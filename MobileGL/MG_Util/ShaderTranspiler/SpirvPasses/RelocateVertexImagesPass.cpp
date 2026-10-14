// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/RelocateVertexImagesPass.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "RelocateVertexImagesPass.h"

#include "spirv.hpp"
#include "source/latest_version_glsl_std_450_header.h"
#include "source/opt/build_module.h"
#include "source/opt/def_use_manager.h"
#include "source/opt/instruction.h"
#include "source/opt/ir_builder.h"
#include "source/opt/ir_context.h"
#include "source/opt/module.h"
#include "source/opt/type_manager.h"
#include "source/util/make_unique.h"
#include "source/util/string_utils.h"

#include <memory>
#include <vector>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            namespace {
                using spvtools::opt::Instruction;
                using spvtools::opt::InstructionBuilder;
                using spvtools::opt::IRContext;

                // OpTypeImage in-operands: 0 sampled type, 1 Dim, 2 Depth, 3 Arrayed, 4 MS,
                // 5 Sampled, 6 Format (same indices ClampMultisampleFetchPass uses).
                constexpr uint32_t kImageDimOperand = 1;
                constexpr uint32_t kImageDepthOperand = 2;
                constexpr uint32_t kImageMsOperand = 4;
                constexpr uint32_t kImageSampledOperand = 5;
                constexpr uint32_t kImageFormatOperand = 6;

                constexpr uint32_t kSampledImage = 2; // OpTypeImage's storage-image shape
                constexpr uint32_t kSampledTexture = 1;

                // OpImageFetch / OpImageRead in-operands: 0 image, 1 coordinate, 2 the optional
                // image-operands mask, 3.. the ids that mask asks for.
                constexpr uint32_t kImageOperandsMaskOperand = 2;

                // The Lod image-operand mask word and the synthesized uniform's prefix. The
                // prefix matches the mg_ convention of the other synthesized uniforms
                // (mg_lodBias_, mg_DrawID, ...), so a driver log names the source.
                constexpr uint32_t kLodImageOperandMask = static_cast<uint32_t>(spv::ImageOperandsMask::Lod);
                constexpr const char* kLodUniformPrefix = "mg_imglod_";

                // The Dims whose SAMPLED spelling has an ESSL texelFetch. Cube is deliberately
                // absent - ESSL has no texelFetch(samplerCube) - and 1D/Rect are handled by the
                // passes further up this chain and would come out as shapes this one declines
                // anyway.
                bool IsRelocatableDim(uint32_t dim) {
                    return dim == static_cast<uint32_t>(spv::Dim::Dim2D) ||
                           dim == static_cast<uint32_t>(spv::Dim::Dim3D) ||
                           dim == static_cast<uint32_t>(spv::Dim::Buffer);
                }

                bool IsStorageImageType(const Instruction* type) {
                    return type != nullptr && type->opcode() == spv::Op::OpTypeImage &&
                           type->NumInOperands() > kImageFormatOperand &&
                           type->GetSingleWordInOperand(kImageSampledOperand) == kSampledImage;
                }

                // The OpName a variable carries, or empty. glslang emits one for every uniform
                // of an OpenGL-profile module - SPIRV-Cross and the ESSL-level passes both key on
                // these names - so empty here means a shape the rest of the pipeline could not
                // name either, and the caller declines rather than minting a name it cannot match
                // the app's view of.
                String GetVariableName(IRContext* context, uint32_t id) {
                    for (const Instruction& debugInst : context->debugs2()) {
                        if (debugInst.opcode() == spv::Op::OpName && debugInst.GetSingleWordInOperand(0) == id) {
                            const auto words = debugInst.GetInOperand(1).words;
                            String name;
                            name.reserve(words.size());
                            for (const uint32_t word : words) {
                                for (int byte = 0; byte < 4; ++byte) {
                                    const char c = static_cast<char>((word >> (byte * 8)) & 0xFFu);
                                    if (c == '\0') return name;
                                    name.push_back(c);
                                }
                            }
                            return name;
                        }
                    }
                    return {};
                }

                // One relocatable uniform: its variable, its image type, every OpLoad that
                // produces the image value, and every OpImageRead on those loads. Query sites are
                // deliberately absent - OpImageQuerySize(Lod) is valid on a Sampled = 1 image and
                // needs no rewrite - but they PARTICIPATE in the use audit below.
                struct RelocationCandidate {
                    Instruction* variable = nullptr;
                    Instruction* imageType = nullptr;
                    struct ReadSite {
                        Instruction* read = nullptr;
                        uint32_t loadedImageId = 0; // the OpLoad result the fetch will read
                    };
                    Vector<ReadSite> reads;
                    String name;
                    uint32_t lodVariableId = 0; // the synthesized mg_imglod_ uniform, phase 2
                };

                // THE USE AUDIT, shared by the probe and the pass. Returns false and names the
                // offender when ANY image uniform of the module has a use this relocation cannot
                // spell: a non-Load use of the variable, a use of a loaded image that is neither
                // OpImageRead nor a size query, a shape no ESSL fetch exists for. Phase discipline
                // is the ClampMultisampleFetch one: this audits ONLY, so a decline leaves the
                // module byte-identical.
                bool AuditVertexImages(IRContext* context, Vector<RelocationCandidate>& candidates,
                                       String& outDeclineReason) {
                    auto* defUseMgr = context->get_def_use_mgr();

                    for (const Instruction& inst : context->module()->types_values()) {
                        if (inst.opcode() != spv::Op::OpVariable) {
                            continue;
                        }
                        // glslang puts an image uniform in UniformConstant (BakeImageFormatsPass
                        // audits the same class); Uniform is accepted too for modules that spell
                        // the opaque differently - both print through SPIRV-Cross identically.
                        const auto storageClass = static_cast<spv::StorageClass>(inst.GetSingleWordInOperand(0));
                        if (storageClass != spv::StorageClass::UniformConstant &&
                            storageClass != spv::StorageClass::Uniform) {
                            continue;
                        }
                        const Instruction* pointerType = defUseMgr->GetDef(inst.type_id());
                        if (pointerType == nullptr || pointerType->opcode() != spv::Op::OpTypePointer) {
                            continue;
                        }
                        const Instruction* pointee = defUseMgr->GetDef(pointerType->GetSingleWordInOperand(1));
                        if (pointee == nullptr) {
                            continue;
                        }
                        // Only a BARE image relocates. An array of images would need one fetch
                        // target per element at units the frontend assigns per element - a shape
                        // no shadow pack's vertex stage uses, declined rather than approximated.
                        if (pointee->opcode() != spv::Op::OpTypeImage) {
                            if (pointee->opcode() == spv::Op::OpTypeArray) {
                                const Instruction* element =
                                    defUseMgr->GetDef(pointee->GetSingleWordInOperand(0));
                                if (element != nullptr && element->opcode() == spv::Op::OpTypeImage &&
                                    element->GetSingleWordInOperand(kImageSampledOperand) == kSampledImage) {
                                    outDeclineReason =
                                        "its type is an array of images, which this relocation does not spell";
                                    return false;
                                }
                            }
                            continue;
                        }
                        if (!IsStorageImageType(pointee)) {
                            continue;
                        }

                        const uint32_t dim = pointee->GetSingleWordInOperand(kImageDimOperand);
                        const uint32_t depth = pointee->GetSingleWordInOperand(kImageDepthOperand);
                        const uint32_t ms = pointee->GetSingleWordInOperand(kImageMsOperand);
                        if (!IsRelocatableDim(dim)) {
                            outDeclineReason = "its dimension has no ESSL texelFetch spelling";
                            return false;
                        }
                        if (ms == 1u) {
                            outDeclineReason = "it is multisampled, and this relocation does not spell fetches "
                                               "with a sample index";
                            return false;
                        }
                        if (depth == 1u) {
                            outDeclineReason = "it is a depth-compare image, which no texelFetch accepts";
                            return false;
                        }

                        const String name = GetVariableName(context, inst.result_id());
                        if (name.empty()) {
                            outDeclineReason = "it carries no OpName, so the fetch mip-level uniform could not "
                                               "be named after it";
                            return false;
                        }

                        RelocationCandidate candidate;
                        candidate.variable = const_cast<Instruction*>(&inst);
                        candidate.imageType = const_cast<Instruction*>(pointee);
                        candidate.name = name;

                        // Every use of the variable must be a plain OpLoad producing the image
                        // value; every use of that value must be a read or a size query.
                        defUseMgr->ForEachUse(inst.result_id(),
                                              [&candidate, &outDeclineReason, defUseMgr](Instruction* user,
                                                                                         uint32_t operandIndex) {
                                                  if (outDeclineReason.empty() && user != nullptr &&
                                                      user->opcode() == spv::Op::OpLoad) {
                                                      defUseMgr->ForEachUse(
                                                          user->result_id(),
                                                          [&candidate, &outDeclineReason, &user](Instruction* imageUser,
                                                                                                 uint32_t imageOperand) {
                                                              if (!outDeclineReason.empty()) return;
                                                              if (imageUser == nullptr || imageOperand != 0u) {
                                                                  outDeclineReason = "a use of the loaded image "
                                                                                     "is not its first operand";
                                                                  return;
                                                              }
                                                              switch (imageUser->opcode()) {
                                                                  case spv::Op::OpImageRead:
                                                                      if (imageUser->NumInOperands() >
                                                                              kImageOperandsMaskOperand &&
                                                                          imageUser->GetSingleWordInOperand(
                                                                              kImageOperandsMaskOperand) != 0u) {
                                                                          outDeclineReason = "an OpImageRead "
                                                                                             "carries image operands "
                                                                                             "this relocation would "
                                                                                             "drop";
                                                                          break;
                                                                      }
                                                                      candidate.reads.push_back(
                                                                          {imageUser, user->result_id()});
                                                                      break;
                                                                  case spv::Op::OpImageQuerySize:
                                                                  case spv::Op::OpImageQuerySizeLod:
                                                                      // Valid on a Sampled = 1 image as-is.
                                                                      break;
                                                                  default:
                                                                      outDeclineReason =
                                                                          "a use of the loaded image is neither an "
                                                                          "OpImageRead nor a size query (writes and "
                                                                          "atomics cannot become texture fetches)";
                                                                      break;
                                                              }
                                                          });
                                                      return;
                                                  }
                                                  if (outDeclineReason.empty()) {
                                                      outDeclineReason = "the image uniform has a use that is not "
                                                                         "a plain OpLoad";
                                                  }
                                              });

                        if (!outDeclineReason.empty()) {
                            outDeclineReason = "image uniform '" + name + "' declines: " + outDeclineReason;
                            return false;
                        }
                        candidates.push_back(std::move(candidate));
                    }
                    return true;
                }
            } // namespace

            spvtools::opt::Pass::Status RelocateVertexImagesPass::Process() {
                auto* irContext = context();
                auto* defUseMgr = irContext->get_def_use_mgr();
                auto* typeMgr = irContext->get_type_mgr();

                Vector<RelocationCandidate> candidates;
                String declineReason;
                if (!AuditVertexImages(irContext, candidates, declineReason)) {
                    MGLOG_D("RelocateVertexImagesPass: %s", declineReason.c_str());
                    return Status::Failure;
                }
                if (candidates.empty()) {
                    return Status::SuccessWithoutChange;
                }

                // One Lod uniform per relocated image, created only after the whole module has
                // audited clean - everything below either completes or fails the pass, never
                // half-rewrites it. The type is the module's own signed int (every coordinate
                // expression already needs one); the pointer type is minted the way
                // LowerViewportIndexPass mints its Private one. Storage class is Uniform, the
                // spelling glslang itself gives a standalone scalar uniform - and the one the
                // validator's block/DescriptorSet rules skip, so the decoration-free variable
                // stays Vulkan-clean.
                const uint32_t intTypeId = typeMgr->GetSIntTypeId();
                if (intTypeId == 0u) {
                    return Status::Failure;
                }
                const uint32_t uniformIntPointerTypeId = typeMgr->FindPointerToType(intTypeId, spv::StorageClass::Uniform);
                if (uniformIntPointerTypeId == 0u) {
                    return Status::Failure;
                }

                for (RelocationCandidate& candidate : candidates) {
                    const uint32_t lodVarId = irContext->TakeNextId();
                    if (lodVarId == 0u) {
                        return Status::Failure;
                    }
                    auto lodVariable = spvtools::MakeUnique<Instruction>(
                        irContext, spv::Op::OpVariable, uniformIntPointerTypeId, lodVarId,
                        Instruction::OperandList{{SPV_OPERAND_TYPE_LITERAL_INTEGER,
                                                  {static_cast<uint32_t>(spv::StorageClass::Uniform)}}});
                    irContext->module()->AddGlobalValue(std::move(lodVariable));
                    irContext->AddDebug2Inst(spvtools::MakeUnique<Instruction>(
                        irContext, spv::Op::OpName, 0, 0,
                        std::initializer_list<spvtools::opt::Operand>{
                            {SPV_OPERAND_TYPE_ID, {lodVarId}},
                            {SPV_OPERAND_TYPE_LITERAL_STRING,
                             spvtools::utils::MakeVector(String(kLodUniformPrefix) + candidate.name)}}));
                    candidate.lodVariableId = lodVarId;
                }

                // Sampled 2 -> 1 and the format qualifier away. The in-place edit is the
                // DemoteFloat64 one: the type's <id> and every pointer to it stay, only its
                // meaning changes.
                for (const RelocationCandidate& candidate : candidates) {
                    candidate.imageType->SetInOperand(kImageSampledOperand, {kSampledTexture});
                    candidate.imageType->SetInOperand(kImageFormatOperand, {0u});
                }

                // The memory decorations describe storage-image access and would be printed onto
                // the sampled spelling, where they are no legal ESSL. DescriptorSet/Binding are
                // stripped with them: the fetch reads a TEXTURE unit the backend assigns at
                // draw time, so a baked layout(binding = N) would both lie about that and risk
                // colliding with an app sampler's binding at link time.
                for (const RelocationCandidate& candidate : candidates) {
                    Vector<Instruction*> deadDecorations;
                    for (Instruction& annotation : irContext->annotations()) {
                        if (annotation.opcode() != spv::Op::OpDecorate ||
                            annotation.GetSingleWordInOperand(0) != candidate.variable->result_id()) {
                            continue;
                        }
                        switch (static_cast<spv::Decoration>(annotation.GetSingleWordInOperand(1))) {
                            case spv::Decoration::RelaxedPrecision:
                            case spv::Decoration::Coherent:
                            case spv::Decoration::NonReadable:
                            case spv::Decoration::NonWritable:
                            case spv::Decoration::Volatile:
                            case spv::Decoration::Restrict:
                            case spv::Decoration::DescriptorSet:
                            case spv::Decoration::Binding:
                                deadDecorations.push_back(&annotation);
                                break;
                            default:
                                break;
                        }
                    }
                    for (Instruction* dead : deadDecorations) {
                        irContext->KillInst(dead);
                    }
                }

                for (const RelocationCandidate& candidate : candidates) {
                    for (const RelocationCandidate::ReadSite& site : candidate.reads) {
                        Instruction* read = site.read;
                        const bool isBuffer =
                            candidate.imageType->GetSingleWordInOperand(kImageDimOperand) ==
                            static_cast<uint32_t>(spv::Dim::Buffer);
                        // A buffer image fetches without a Lod operand (and its texelFetch has
                        // none to print); every other accepted Dim is required to carry one, and
                        // it comes from the synthesized uniform so the backend can follow the
                        // image binding's level.
                        Instruction::OperandList operands{
                            {SPV_OPERAND_TYPE_ID, {site.loadedImageId}},
                            {SPV_OPERAND_TYPE_ID, {read->GetSingleWordInOperand(1)}},
                        };
                        if (!isBuffer) {
                            operands.push_back(
                                {SPV_OPERAND_TYPE_LITERAL_INTEGER, {kLodImageOperandMask}});
                            operands.push_back({SPV_OPERAND_TYPE_ID, {candidate.lodVariableId}});
                        }

                        InstructionBuilder builder(
                            irContext, read, IRContext::kAnalysisDefUse | IRContext::kAnalysisInstrToBlockMapping);
                        Instruction* fetch = builder.AddInstruction(spvtools::MakeUnique<Instruction>(
                            irContext, spv::Op::OpImageFetch, read->type_id(), read->result_id(), std::move(operands)));
                        if (fetch == nullptr) {
                            return Status::Failure;
                        }
                        irContext->KillInst(read);
                    }
                }

                irContext->InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);
                return Status::SuccessWithChange;
            }

            spvtools::Optimizer::PassToken RelocateVertexImagesPass::CreateRelocateVertexImagesPass() {
                return spvtools::Optimizer::PassToken(spvtools::MakeUnique<RelocateVertexImagesPass>());
            }

            bool AuditVertexImageRelocation(spvtools::opt::IRContext* context,
                                            Uint32& outCandidateCount, String& outDeclineReason) {
                outCandidateCount = 0;
                outDeclineReason.clear();
                Vector<RelocationCandidate> candidates;
                if (!AuditVertexImages(context, candidates, outDeclineReason)) {
                    return false;
                }
                outCandidateCount = static_cast<Uint32>(candidates.size());
                return true;
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
