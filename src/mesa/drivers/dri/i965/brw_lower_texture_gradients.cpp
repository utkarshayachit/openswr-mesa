/*
 * Copyright © 2012 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/**
 * \file brw_lower_texture_gradients.cpp
 */

#include "glsl/ir.h"
#include "glsl/ir_builder.h"
#include "program/prog_instruction.h"
#include "brw_context.h"

using namespace ir_builder;

class lower_texture_grad_visitor : public ir_hierarchical_visitor {
public:
   lower_texture_grad_visitor(bool has_sample_d_c)
      : has_sample_d_c(has_sample_d_c)
   {
      progress = false;
   }

   ir_visitor_status visit_leave(ir_texture *ir);


   bool progress;
   bool has_sample_d_c;

private:
   void emit(ir_variable *, ir_rvalue *);
};

/**
 * Emit a variable declaration and an assignment to initialize it.
 */
void
lower_texture_grad_visitor::emit(ir_variable *var, ir_rvalue *value)
{
   base_ir->insert_before(var);
   base_ir->insert_before(assign(var, value));
}

static const glsl_type *
txs_type(const glsl_type *type)
{
   unsigned dims;
   switch (type->sampler_dimensionality) {
   case GLSL_SAMPLER_DIM_1D:
      dims = 1;
      break;
   case GLSL_SAMPLER_DIM_2D:
   case GLSL_SAMPLER_DIM_RECT:
   case GLSL_SAMPLER_DIM_CUBE:
      dims = 2;
      break;
   case GLSL_SAMPLER_DIM_3D:
      dims = 3;
      break;
   default:
      unreachable("Should not get here: invalid sampler dimensionality");
   }

   if (type->sampler_array)
      dims++;

   return glsl_type::get_instance(GLSL_TYPE_INT, dims, 1);
}

ir_visitor_status
lower_texture_grad_visitor::visit_leave(ir_texture *ir)
{
   /* Only lower textureGrad with cube maps or shadow samplers */
   if (ir->op != ir_txd ||
      (ir->sampler->type->sampler_dimensionality != GLSL_SAMPLER_DIM_CUBE &&
       !ir->shadow_comparitor))
      return visit_continue;

   /* Lower textureGrad() with samplerCube* even if we have the sample_d_c
    * message.  GLSL provides gradients for the 'r' coordinate.  Unfortunately:
    *
    * From the Ivybridge PRM, Volume 4, Part 1, sample_d message description:
    * "The r coordinate contains the faceid, and the r gradients are ignored
    *  by hardware."
    */
   bool need_lowering = !has_sample_d_c ||
      ir->sampler->type->sampler_dimensionality == GLSL_SAMPLER_DIM_CUBE;

   if (!need_lowering)
      return visit_continue;

   void *mem_ctx = ralloc_parent(ir);

   const glsl_type *grad_type = ir->lod_info.grad.dPdx->type;

   /* Use textureSize() to get the width and height of LOD 0; swizzle away
    * the depth/number of array slices.
    */
   ir_texture *txs = new(mem_ctx) ir_texture(ir_txs);
   txs->set_sampler(ir->sampler->clone(mem_ctx, NULL),
		    txs_type(ir->sampler->type));
   txs->lod_info.lod = new(mem_ctx) ir_constant(0);
   ir_variable *size =
      new(mem_ctx) ir_variable(grad_type, "size", ir_var_temporary);
   if (ir->sampler->type->sampler_dimensionality == GLSL_SAMPLER_DIM_CUBE) {
      base_ir->insert_before(size);
      base_ir->insert_before(assign(size,
                                    swizzle_for_size(expr(ir_unop_i2f, txs), 2),
                                    WRITEMASK_XY));
      base_ir->insert_before(assign(size, new(mem_ctx) ir_constant(1.0f), WRITEMASK_Z));
   } else {
      emit(size, expr(ir_unop_i2f,
                      swizzle_for_size(txs, grad_type->vector_elements)));
   }

   /* Scale the gradients by width and height.  Effectively, the incoming
    * gradients are s'(x,y), t'(x,y), and r'(x,y) from equation 3.19 in the
    * GL 3.0 spec; we want u'(x,y), which is w_t * s'(x,y).
    */
   ir_variable *dPdx =
      new(mem_ctx) ir_variable(grad_type, "dPdx", ir_var_temporary);
   emit(dPdx, mul(size, ir->lod_info.grad.dPdx));

   ir_variable *dPdy =
      new(mem_ctx) ir_variable(grad_type, "dPdy", ir_var_temporary);
   emit(dPdy, mul(size, ir->lod_info.grad.dPdy));

   /* Calculate rho from equation 3.20 of the GL 3.0 specification. */
   ir_rvalue *rho;
   if (dPdx->type->is_scalar()) {
      rho = expr(ir_binop_max, expr(ir_unop_abs, dPdx),
			       expr(ir_unop_abs, dPdy));
   } else {
      rho = expr(ir_binop_max, expr(ir_unop_sqrt, dot(dPdx, dPdx)),
			       expr(ir_unop_sqrt, dot(dPdy, dPdy)));
   }

   /* lambda_base = log2(rho).  We're ignoring GL state biases for now.
    *
    * For cube maps the result of these formulas is giving us a value of rho
    * that is twice the value we should use, so divide it by 2 or,
    * alternatively, remove one unit from the result of the log2 computation.
    */
   ir->op = ir_txl;
   if (ir->sampler->type->sampler_dimensionality == GLSL_SAMPLER_DIM_CUBE) {
      ir->lod_info.lod = expr(ir_binop_add,
                              expr(ir_unop_log2, rho),
                              new(mem_ctx) ir_constant(-1.0f));
   } else {
      ir->lod_info.lod = expr(ir_unop_log2, rho);
   }

   progress = true;
   return visit_continue;
}

extern "C" {

bool
brw_lower_texture_gradients(struct brw_context *brw,
                            struct exec_list *instructions)
{
   bool has_sample_d_c = brw->gen >= 8 || brw->is_haswell;
   lower_texture_grad_visitor v(has_sample_d_c);

   visit_list_elements(&v, instructions);

   return v.progress;
}

}
