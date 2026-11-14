#pragma once

#include <cstddef>
#include <cstdint>

#include <NetImmerse/NiPointer.h>
#include <NetImmerse/NiTexture.h>

struct BSMaskedShaderMaterial
{
    static constexpr uint32_t kFeatureFaceGen = 4u;
    static constexpr uint32_t kTypeLighting = 2u;

    virtual ~BSMaskedShaderMaterial();
    virtual BSMaskedShaderMaterial* Create();
    virtual void Copy(BSMaskedShaderMaterial* apSource);
    virtual bool DoIsCopy(BSMaskedShaderMaterial* apOther);
    virtual uint32_t ComputeCRC32();
    virtual BSMaskedShaderMaterial* GetDefault();
    virtual uint32_t GetFeature() const;
    virtual uint32_t GetType() const;

    uint8_t pad08[0x98];

    NiPointer<NiTexture> renderedTexture;
};

static_assert(offsetof(BSMaskedShaderMaterial, renderedTexture) == 0xA0);
