// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/RelocateVertexImagesPass.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include "source/opt/pass.h"
#include "spirv-tools/optimizer.hpp"

#include <Includes.h>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            // Rewrites every storage image (OpTypeImage with Sampled = 2) the VERTEX stage of a
            // module reads into the sampled shape (Sampled = 1), and every OpImageRead on it into
            // OpImageFetch with a Lod image-operand fed by a synthesized `uniform int
            // mg_imglod_<name>`. SPIRV-Cross then prints exactly what glslang would have printed
            // for a hand-written `texelFetch(sampler2D, coord, mg_imglod_name)` - and vertex
            // texture fetch is universally supported, while vertex IMAGE uniforms are not.
            //
            // WHY: desktop GLSL lets a vertex stage imageLoad() like any other stage, and Solas /
            // iterationT shadow passes do exactly that (voxel lookups per shadow vertex). GLES
            // drivers are free to advertise GL_MAX_VERTEX_IMAGE_UNIFORMS = 0 and Mali's r38p1
            // does, so the ESSL link fails ("more than the 0 GL_MAX_VERTEX_IMAGE_UNIFORMS
            // allows") and every shadow program of those packs is lost. The data the shader wants
            // is an ordinary texture at an image unit - so the read becomes a fetch through a
            // plain sampler, and the image unit's texture is bound at a reserved TEXTURE unit by
            // the backend (PrgramImpl::RelocatedImageBinding). Mip level: imageLoad reads the
            // level the image binding named, texelFetch names one explicitly - hence the uniform,
            // driven per draw from the binding's Level, on the same shape as the LOD-bias
            // emulation. Shader packs bind their voxel images at level 0, so the uniform sits at
            // 0 for them; the mechanism exists so a level-k binding stays byte-correct.
            //
            // WHAT IS CONVERTIBLE, strictly. An image uniform (a UniformConstant - or Uniform,
            // for modules that spell the opaque there - variable over an OpTypeImage with
            // Sampled = 2) is relocated only when EVERY use in the module is OpLoad ->
            // (OpImageRead | OpImageQuerySize | OpImageQuerySizeLod), the image is not
            // multisampled and not depth-compare, and its Dim is 2D, 3D or Buffer - the shapes
            // whose sampled spelling is expressible ESSL 3.20. Cube declines: ESSL has no
            // texelFetch(samplerCube). The 1D shapes Lower1DSampledImagesPass already consumed
            // further up this chain would come out as shapes this one declines anyway. Writes,
            // atomics (OpImageTexelPointer), image arrays, function parameters and anything
            // else DECLINE the whole pass: a half-rewritten module would read some images as
            // textures and let the driver reject others. The caller turns a decline into a
            // transpile error naming the variable, which is honest - such a stage has no
            // driver-visible spelling at all when the advertised vertex image count is 0.
            //
            // The image's ImageFormat operand is cleared with Sampled: GLSL has no format on a
            // sampler, and the frontend state that gave the image its format (the bake/widen
            // passes upstream) has no counterpart on the fetch path - the bytes fetched are the
            // texture's own, whatever the binding named.
            //
            // Memory decorations (readonly/writeonly/coherent/volatile/restrict) are killed with
            // the image uses: they describe storage-image access, and SPIRV-Cross would print
            // them on a `sampler2D`, which is no legal ESSL.
            //
            // ORDERING: runs inside the DirectGLES transpile chain AFTER the image-format bake
            // and widen passes (it consumes their final shapes) and BEFORE SPIRV-Cross. It is
            // ARMED by g_GLESCapabilities.MaxVertexImageUniforms == 0 only, and that bit is L2
            // key material (EsslTranslationKeyInputs::relocateVertexImagesArmed) - a module
            // relocated for a Mali must never be served to a driver that advertises images.
            // DirectVulkan never runs it: its image uniforms become descriptors the hardware
            // loads in any stage.
            class RelocateVertexImagesPass : public spvtools::opt::Pass {
            public:
                const char* name() const override { return "mobilegl-relocate-vertex-images"; }
                Status Process() override;

                static spvtools::Optimizer::PassToken CreateRelocateVertexImagesPass();
            };

            // Audit-only entry the transpile wrapper reaches for its decline reporting: runs the
            // pass's use audit over an ALREADY-BUILT context and reports the first decline with
            // the offending uniform named, without mutating the module. `outCandidateCount`
            // receives the number of relocatable image uniforms found; a decline with a non-empty
            // reason also implies the module declares a storage image (the offender). False only
            // when a decline was reported.
            bool AuditVertexImageRelocation(spvtools::opt::IRContext* context,
                                            Uint32& outCandidateCount, String& outDeclineReason);
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
