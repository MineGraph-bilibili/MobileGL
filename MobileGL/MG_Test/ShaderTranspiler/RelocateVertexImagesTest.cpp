// MobileGL - MobileGL/MG_Test/ShaderTranspiler/RelocateVertexImagesTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// RelocateVertexImagesPass exists because a Mali driver reports
// GL_MAX_VERTEX_IMAGE_UNIFORMS = 0, and MobileGL's own link check enforces the
// advertised numbers - so a shaderpack whose VERTEX stage reads a storage image
// (Solas, iterationT) failed to link on such a device while loading cleanly
// everywhere else. The relocation carries a vertex-stage image onto the texture
// pipeline instead: the declaration becomes a Sampled = 1 image (which
// SPIRV-Cross prints as a sampler2D), every OpImageRead becomes an
// OpImageFetch whose Lod operand reads a synthesized mg_imglod_<name> scalar
// uniform, and the backend binds the image unit's texture onto a reserved unit
// at draw time.
//
// What has to hold: a plain fetch relocates exactly (sampler spelling, fetch
// with the Lod operand, no storage-image shape left behind), the audit refuses
// the forms no ESSL fetch can spell - writes, atomics, multisampled, cube,
// arrays - without touching the module, and a shader with no images at all
// passes through byte-identical. Real GLSL through the same glslang path the
// backends use, for the same reason WidenImageFormatsTest.cpp does it.
//
// One deliberate difference from the other pass suites: the RELOCATED module is
// not asserted to validate under SPV_ENV_VULKAN_1_1. Stripping
// DescriptorSet/Binding off the (still UniformConstant) variable is what keeps
// SPIRV-Cross printing a bare sampler the backend can point at any texture
// unit, and the validator refuses exactly that - val/validate_decorations
// requires both decorations on an entry-point-referenced UniformConstant. The
// shipping pipeline runs ValidateOrLatch here, which records and does not
// block, and the ESSL emission is the real end-to-end gate these tests
// exercise.

#include <gtest/gtest.h>

#define SPV_ENABLE_UTILITY_CODE
#include "glslang/SPIRV/spirv.hpp11"
#undef SPV_ENABLE_UTILITY_CODE

#include "Includes.h"
#include "Init.h"
#include <MG_Util/ShaderTranspiler/ShaderCompiler.h>
#include <MG_Util/ShaderTranspiler/SpvcSession.h>
#include <MG_Util/ShaderTranspiler/Types.h>

#include <spirv-tools/libspirv.hpp>

#include <string>
#include <vector>

using namespace MobileGL;
using MobileGL::MG_Util::ShaderTranspiler::ShaderCompiler;

namespace {
    constexpr SizeT kSpirvHeaderWordCount = 5u;

    template <typename Visitor>
    void ForEachInstruction(const Vector<Uint32>& spirv, Visitor&& visit) {
        for (SizeT offset = kSpirvHeaderWordCount; offset < spirv.size();) {
            const Uint32 wordCount = spirv[offset] >> 16u;
            if (wordCount == 0u || offset + wordCount > spirv.size()) break;
            visit(static_cast<spv::Op>(spirv[offset] & 0xffffu), &spirv[offset], wordCount);
            offset += wordCount;
        }
    }

    Vector<Uint32> CompileVertex(const String& source) {
        using namespace MobileGL::MG_Util::ShaderTranspiler;
        ShaderAttrib shaderAttrib{.shaderType = GL_VERTEX_SHADER, .sourceStr = source};
        auto shaderResult = ShaderCompiler::CompileShader(shaderAttrib);
        EXPECT_TRUE(shaderResult) << (shaderResult ? String{} : shaderResult.error().log);
        if (!shaderResult) return {};

        ProgramAttrib programAttrib{.shaders = {shaderResult.value()}};
        auto programResult = ShaderCompiler::LinkProgram(programAttrib);
        EXPECT_TRUE(programResult) << (programResult ? String{} : programResult.error().log);
        if (!programResult) return {};

        ProgramBinaryAttrib binaryAttrib{.shaderTypes = {GL_VERTEX_SHADER},
                                         .program = *programResult.value()};
        auto binaryResult = ShaderCompiler::GetSpirvBinaryFromProgram(binaryAttrib);
        EXPECT_TRUE(binaryResult) << (binaryResult ? String{} : binaryResult.error().log);
        if (!binaryResult || binaryResult->empty()) return {};
        return binaryResult->front();
    }

    // OpTypeImage words: 0 opcode/count, 1 result id, 2 sampled type, 3 Dim, 4 Depth, 5 Arrayed,
    // 6 MS, 7 Sampled, 8 Format. Sampled == 2 is a storage image, the only kind with a format.
    struct ImageType {
        Uint32 resultId = 0u;
        Uint32 dim = 0u;
        Uint32 ms = 0u;
        Uint32 sampled = 0u;
        Uint32 format = 0u;
    };

    Vector<ImageType> CollectImageTypes(const Vector<Uint32>& spirv) {
        Vector<ImageType> types;
        ForEachInstruction(spirv, [&](spv::Op opcode, const Uint32* words, Uint32 wordCount) {
            if (opcode != spv::Op::OpTypeImage || wordCount < 9u) return;
            types.push_back(ImageType{words[1], words[3], words[6], words[7], words[8]});
        });
        return types;
    }

    SizeT CountStorageImageTypes(const Vector<Uint32>& spirv) {
        SizeT count = 0;
        for (const ImageType& type : CollectImageTypes(spirv)) {
            if (type.sampled == 2u) ++count;
        }
        return count;
    }

    // OpImageFetch words: 0 opcode/count, 1 result type, 2 result id, 3 image, 4 coordinate,
    // 5.. the optional image-operands mask and the ids it asks for.
    struct Fetch {
        Uint32 imageId = 0u;
        Uint32 coordinateId = 0u;
        Uint32 maskWord = 0u;
        Uint32 lodId = 0u;
        Uint32 operandCount = 0u;
    };

    Vector<Fetch> CollectFetches(const Vector<Uint32>& spirv) {
        Vector<Fetch> fetches;
        ForEachInstruction(spirv, [&](spv::Op opcode, const Uint32* words, Uint32 wordCount) {
            if (opcode != spv::Op::OpImageFetch || wordCount < 5u) return;
            Fetch fetch{};
            fetch.imageId = words[3];
            fetch.coordinateId = words[4];
            fetch.operandCount = wordCount - 5u;
            if (fetch.operandCount >= 1u) fetch.maskWord = words[5];
            if (fetch.operandCount >= 2u) fetch.lodId = words[6];
            fetches.push_back(fetch);
        });
        return fetches;
    }

    SizeT CountOpImageReads(const Vector<Uint32>& spirv) {
        SizeT count = 0;
        ForEachInstruction(spirv, [&](spv::Op opcode, const Uint32*, Uint32) {
            if (opcode == spv::Op::OpImageRead) ++count;
        });
        return count;
    }

    Vector<String> CollectOpNames(const Vector<Uint32>& spirv) {
        Vector<String> names;
        ForEachInstruction(spirv, [&](spv::Op opcode, const Uint32* words, Uint32) {
            if (opcode != spv::Op::OpName) return;
            // Words: 0 opcode/count, 1 target id, 2.. the literal string.
            String name;
            for (Uint32 word = 2u; word < (words[0] >> 16u); ++word) {
                for (int byte = 0; byte < 4; ++byte) {
                    const char c = static_cast<char>((words[word] >> (byte * 8)) & 0xFFu);
                    if (c == '\0') {
                        names.push_back(name);
                        return;
                    }
                    name.push_back(c);
                }
            }
            names.push_back(name);
        });
        return names;
    }

    bool HasName(const Vector<String>& names, const char* expected) {
        for (const String& name : names) {
            if (name == expected) return true;
        }
        return false;
    }

    // The storage class a global OpVariable declares, or 0xFFFFFFFF when the id is not one.
    Uint32 StorageClassOf(const Vector<Uint32>& spirv, Uint32 variableId) {
        Uint32 storageClass = 0xFFFFFFFFu;
        ForEachInstruction(spirv, [&](spv::Op opcode, const Uint32* words, Uint32 wordCount) {
            if (opcode != spv::Op::OpVariable || wordCount < 4u || words[2] != variableId) return;
            storageClass = words[3];
        });
        return storageClass;
    }

    bool HasDescriptorOrBindingDecoration(const Vector<Uint32>& spirv, const char* variableName) {
        // Resolve the name to an id first, then look for any OpDecorate DescriptorSet/Binding.
        Uint32 targetId = 0u;
        const Vector<String> names = CollectOpNames(spirv);
        for (SizeT i = 0; i < names.size(); ++i) {
            if (names[i] == variableName) {
                // OpName order matches its target order; re-walk to recover the id.
                Uint32 index = 0;
                ForEachInstruction(spirv, [&](spv::Op opcode, const Uint32* words, Uint32) {
                    if (opcode != spv::Op::OpName) return;
                    if (index++ == i) targetId = words[1];
                });
            }
        }
        if (targetId == 0u) return false;
        bool decorated = false;
        ForEachInstruction(spirv, [&](spv::Op opcode, const Uint32* words, Uint32) {
            if (opcode != spv::Op::OpDecorate || words[1] != targetId) return;
            const auto decoration = static_cast<spv::Decoration>(words[2]);
            if (decoration == spv::Decoration::DescriptorSet || decoration == spv::Decoration::Binding) {
                decorated = true;
            }
        });
        return decorated;
    }

    // The ESSL SPIRV-Cross emits for a module, or the error it refused with.
    struct EsslAttempt {
        Bool succeeded = false;
        String text;
        String error;
    };

    EsslAttempt EmitEssl(const Vector<Uint32>& spirv) {
        using namespace MobileGL::MG_Util::ShaderTranspiler;
        EsslAttempt attempt;
        SpvcSession session(spirv, SessionUsageBit::Transpile);
        spvc_compiler_options options;
        if (session.CreateOptions(&options) != SPVC_SUCCESS) return attempt;
        spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_GLSL_VERSION, 320);
        spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_ES, SPVC_TRUE);
        spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_VULKAN_SEMANTICS, SPVC_FALSE);
        if (session.SetOptions(options) != SPVC_SUCCESS) return attempt;
        auto essl = ShaderCompiler::DecompileShader(session);
        if (!essl) {
            attempt.error = essl.error().log;
            return attempt;
        }
        attempt.succeeded = true;
        attempt.text = *essl;
        return attempt;
    }

    // The shape every shadowpack vertex stage that broke on Mali takes: one image2D the
    // shader only READS, indexed from gl_VertexID.
    const char* const kVertexImageRead = R"(#version 430 core
layout(r32f, binding = 0) uniform image2D img;
out vec4 vColor;
void main() {
    vec4 texel = imageLoad(img, ivec2(gl_VertexID & 63, gl_VertexID >> 6));
    vColor = texel;
    gl_Position = vec4(texel.xy, 0.0, 1.0);
}
)";

    const char* const kVertexImageStore = R"(#version 430 core
layout(r32f, binding = 0) uniform image2D img;
void main() {
    imageStore(img, ivec2(gl_VertexID & 63, gl_VertexID >> 6), vec4(1.0));
}
)";

    const char* const kVertexImageCube = R"(#version 430 core
layout(r32f, binding = 0) uniform imageCube img;
out vec4 vColor;
void main() {
    vec4 texel = imageLoad(img, ivec3(gl_VertexID & 63, (gl_VertexID >> 6) & 63, gl_VertexID >> 12));
    vColor = texel;
    gl_Position = vec4(texel.xy, 0.0, 1.0);
}
)";

    const char* const kVertexImageMs = R"(#version 430 core
layout(r32f, binding = 0) uniform image2DMS img;
out vec4 vColor;
void main() {
    vec4 texel = imageLoad(img, ivec2(gl_VertexID & 63, gl_VertexID >> 6), 0);
    vColor = texel;
    gl_Position = vec4(texel.xy, 0.0, 1.0);
}
)";

    const char* const kVertexImageArray = R"(#version 430 core
layout(r32f, binding = 0) uniform image2D imgs[4];
out vec4 vColor;
void main() {
    vec4 texel = imageLoad(imgs[gl_VertexID & 3], ivec2(gl_VertexID & 63, gl_VertexID >> 6));
    vColor = texel;
    gl_Position = vec4(texel.xy, 0.0, 1.0);
}
)";

    const char* const kVertexNoImages = R"(#version 430 core
layout(binding = 0) uniform sampler2D tex;
out vec4 vColor;
void main() {
    vColor = texture(tex, vec2(0.5));
    gl_Position = vec4(gl_VertexID * 0.001, 0.0, 0.0, 1.0);
}
)";
} // namespace

// The case the whole pass exists for: a vertex image the shader only reads.
TEST(RelocateVertexImages, VertexImageFetchBecomesATexelFetchOnASampler) {
    const Vector<Uint32> spirv = CompileVertex(kVertexImageRead);
    ASSERT_FALSE(spirv.empty());
    ASSERT_EQ(CountStorageImageTypes(spirv), 1u) << "the fixture stopped declaring a storage image";

    const auto probe = ShaderCompiler::ProbeVertexImageRelocation(spirv);
    EXPECT_TRUE(probe.DeclaresStorageImage);
    EXPECT_TRUE(probe.Relocatable);
    EXPECT_TRUE(probe.DeclineReason.empty());

    Vector<Uint32> relocated;
    ASSERT_TRUE(ShaderCompiler::RelocateVertexImagesForEssl(spirv, relocated, /*enableSpirvValidation=*/false));
    ASSERT_FALSE(relocated.empty());
    EXPECT_NE(relocated, spirv) << "a relocatable module must come out changed";

    // The declaration is a sampled spelling now: Sampled 2 -> 1, the format qualifier gone.
    const auto imageTypes = CollectImageTypes(relocated);
    ASSERT_EQ(imageTypes.size(), 1u) << "relocation must not add or drop image types";
    EXPECT_EQ(imageTypes.front().sampled, 1u);
    EXPECT_EQ(imageTypes.front().format, 0u) << "a Sampled = 1 image carries no format qualifier";
    EXPECT_EQ(CountStorageImageTypes(relocated), 0u);

    // The read is a fetch, with the Lod operand reading a synthesized Uniform int.
    EXPECT_EQ(CountOpImageReads(relocated), 0u);
    const auto fetches = CollectFetches(relocated);
    ASSERT_EQ(fetches.size(), 1u);
    EXPECT_EQ(fetches.front().operandCount, 2u) << "a 2D fetch must carry the Lod mask and its id";
    EXPECT_EQ(fetches.front().maskWord, static_cast<Uint32>(spv::ImageOperandsMask::Lod));
    EXPECT_NE(fetches.front().lodId, 0u);
    EXPECT_EQ(StorageClassOf(relocated, fetches.front().lodId),
              static_cast<Uint32>(spv::StorageClass::Uniform))
        << "the Lod uniform must sit in Uniform, the storage class the Vulkan validator's "
           "decoration rules skip for non-blocks";

    // Named after the image, so the backend can find it and a driver log names the source.
    const auto names = CollectOpNames(relocated);
    EXPECT_TRUE(HasName(names, "mg_imglod_img"));

    // The binding decorations are gone - that is what makes SPIRV-Cross print a bare
    // sampler the backend can point at a reserved unit.
    EXPECT_FALSE(HasDescriptorOrBindingDecoration(relocated, "img"));

    // ...and what reaches the driver: a sampler fetched by texel level.
    const EsslAttempt essl = EmitEssl(relocated);
    ASSERT_TRUE(essl.succeeded) << essl.error;
    EXPECT_NE(essl.text.find("sampler2D"), String::npos) << essl.text;
    EXPECT_NE(essl.text.find("texelFetch"), String::npos) << essl.text;
    EXPECT_NE(essl.text.find("mg_imglod_img"), String::npos) << essl.text;
    EXPECT_EQ(essl.text.find("imageLoad"), String::npos) << essl.text;
}

// A write is the one use no texture fetch can spell: texelFetch reads, nothing
// writes. The audit must refuse the module BEFORE anything is rewritten - and
// the probe must say exactly that, because the backend's error path builds its
// message from it.
TEST(RelocateVertexImages, VertexImageStoreDeclinesAndLeavesTheModuleAlone) {
    const Vector<Uint32> spirv = CompileVertex(kVertexImageStore);
    ASSERT_FALSE(spirv.empty());

    const auto probe = ShaderCompiler::ProbeVertexImageRelocation(spirv);
    EXPECT_TRUE(probe.DeclaresStorageImage);
    EXPECT_FALSE(probe.Relocatable);
    EXPECT_FALSE(probe.DeclineReason.empty());

    Vector<Uint32> relocated;
    EXPECT_FALSE(ShaderCompiler::RelocateVertexImagesForEssl(spirv, relocated, false));
    EXPECT_TRUE(relocated.empty()) << "a declined module must not be half-rewritten";
    // Phase discipline: the failed pass must not have touched the input either.
    EXPECT_EQ(CountStorageImageTypes(spirv), 1u);
    EXPECT_EQ(CountOpImageReads(spirv), 0u);
}

TEST(RelocateVertexImages, CubeVertexImageDeclines) {
    const Vector<Uint32> spirv = CompileVertex(kVertexImageCube);
    ASSERT_FALSE(spirv.empty());

    const auto probe = ShaderCompiler::ProbeVertexImageRelocation(spirv);
    EXPECT_TRUE(probe.DeclaresStorageImage);
    EXPECT_FALSE(probe.Relocatable);
    // ESSL has no texelFetch(samplerCube); the reason should say so rather than
    // leaving the caller a bare failure.
    EXPECT_NE(probe.DeclineReason.find("texelFetch"), String::npos) << probe.DeclineReason;

    Vector<Uint32> relocated;
    EXPECT_FALSE(ShaderCompiler::RelocateVertexImagesForEssl(spirv, relocated, false));
    EXPECT_TRUE(relocated.empty());
}

TEST(RelocateVertexImages, MultisampledVertexImageDeclines) {
    const Vector<Uint32> spirv = CompileVertex(kVertexImageMs);
    ASSERT_FALSE(spirv.empty());

    const auto probe = ShaderCompiler::ProbeVertexImageRelocation(spirv);
    EXPECT_TRUE(probe.DeclaresStorageImage);
    EXPECT_FALSE(probe.Relocatable);
    EXPECT_FALSE(probe.DeclineReason.empty());

    Vector<Uint32> relocated;
    EXPECT_FALSE(ShaderCompiler::RelocateVertexImagesForEssl(spirv, relocated, false));
    EXPECT_TRUE(relocated.empty());
}

TEST(RelocateVertexImages, VertexImageArrayDeclines) {
    const Vector<Uint32> spirv = CompileVertex(kVertexImageArray);
    ASSERT_FALSE(spirv.empty());

    const auto probe = ShaderCompiler::ProbeVertexImageRelocation(spirv);
    EXPECT_TRUE(probe.DeclaresStorageImage);
    EXPECT_FALSE(probe.Relocatable);
    EXPECT_NE(probe.DeclineReason.find("array"), String::npos) << probe.DeclineReason;

    Vector<Uint32> relocated;
    EXPECT_FALSE(ShaderCompiler::RelocateVertexImagesForEssl(spirv, relocated, false));
    EXPECT_TRUE(relocated.empty());
}

// The cheap path that keeps every ordinary shader off the optimizer: a vertex
// stage of samplers only is not relocated, and the probe says so without
// building a decline reason.
TEST(RelocateVertexImages, VertexShaderWithoutImagesIsUntouched) {
    const Vector<Uint32> spirv = CompileVertex(kVertexNoImages);
    ASSERT_FALSE(spirv.empty());
    EXPECT_EQ(CountStorageImageTypes(spirv), 0u);

    const auto probe = ShaderCompiler::ProbeVertexImageRelocation(spirv);
    EXPECT_FALSE(probe.DeclaresStorageImage);
    EXPECT_FALSE(probe.Relocatable);
    EXPECT_TRUE(probe.DeclineReason.empty());

    Vector<Uint32> relocated;
    ASSERT_TRUE(ShaderCompiler::RelocateVertexImagesForEssl(spirv, relocated, false));
    EXPECT_TRUE(relocated.empty() || relocated == spirv)
        << "a module with nothing to relocate must come out untouched";
    EXPECT_EQ(CollectFetches(relocated.empty() ? spirv : relocated).size(),
              CollectFetches(spirv).size())
        << "no fetch may appear where the shader had none";
}
