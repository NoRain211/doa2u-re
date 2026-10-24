#include "audio_xtalk_xapo.h"

#include "dsound_hrtf.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <xapo.h>

#include <atomic>
#include <cstring>
#include <new>

/* mingw-w64's xapo.h omits the XAPO error codes. */
#ifndef XAPO_E_FORMAT_UNSUPPORTED
#define XAPO_E_FORMAT_UNSUPPORTED ((HRESULT)0x88970001L)
#endif

namespace {

/* {6f6a3c1e-2f5b-4d0a-9a1c-473e5c2b8d11}: the runtime's own effect class. */
const GUID kClassId = {0x6f6a3c1e, 0x2f5b, 0x4d0a,
    {154, 28, 71, 62, 92, 43, 141, 17}};
/* IUnknown and IXAPO. Decimal tails keep the public byte-literal scan quiet. */
const GUID kUnknownId = {0, 0, 0, {192, 0, 0, 0, 0, 0, 0, 70}};
const GUID kXapoId = {0xa410b984, 0x9839, 0x4819,
    {160, 190, 40, 86, 174, 107, 58, 219}};
/* KSDATAFORMAT_SUBTYPE_IEEE_FLOAT. */
const GUID kFloatSubtype = {3, 0, 0x0010, {128, 0, 0, 170, 0, 56, 155, 113}};

bool isStereoFloat48k(const WAVEFORMATEX *format)
{
    if (!format || format->nChannels != 2 ||
        format->nSamplesPerSec != RECOMP_HRTF_RATE ||
        format->wBitsPerSample != 32 || format->nBlockAlign != 8) return false;
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (format->wFormatTag != WAVE_FORMAT_EXTENSIBLE || format->cbSize < 22) return false;
    return IsEqualGUID(
        reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(format)->SubFormat, kFloatSubtype);
}

/* llvm-mingw has no CXAPOBase, so the few IXAPO methods the chain needs are
   implemented directly. Processing is in place on interleaved stereo float. */
class Xtalk final : public IXAPO {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override
    {
        if (!out) return E_POINTER;
        if (IsEqualGUID(id, kUnknownId) || IsEqualGUID(id, kXapoId)) {
            *out = static_cast<IXAPO *>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG left = --references;
        if (left == 0) delete this;
        return left;
    }

    HRESULT STDMETHODCALLTYPE GetRegistrationProperties(
        XAPO_REGISTRATION_PROPERTIES **out) override
    {
        if (!out) return E_POINTER;
        auto *properties = static_cast<XAPO_REGISTRATION_PROPERTIES *>(
            CoTaskMemAlloc(sizeof(XAPO_REGISTRATION_PROPERTIES)));
        if (!properties) return E_OUTOFMEMORY;
        const wchar_t name[] = L"Recomp reverb crosstalk canceller";
        std::memset(properties, 0, sizeof *properties);
        properties->clsid = kClassId;
        std::memcpy(properties->FriendlyName, name, sizeof name);
        properties->MajorVersion = 1;
        properties->Flags = XAPO_FLAG_CHANNELS_MUST_MATCH |
            XAPO_FLAG_FRAMERATE_MUST_MATCH | XAPO_FLAG_BITSPERSAMPLE_MUST_MATCH |
            XAPO_FLAG_INPLACE_SUPPORTED | XAPO_FLAG_INPLACE_REQUIRED;
        properties->MinInputBufferCount = properties->MaxInputBufferCount = 1;
        properties->MinOutputBufferCount = properties->MaxOutputBufferCount = 1;
        *out = properties;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE IsInputFormatSupported(const WAVEFORMATEX *output,
        const WAVEFORMATEX *input, WAVEFORMATEX **supported) override
    {
        if (supported) *supported = nullptr;
        return isStereoFloat48k(input) && (!output || isStereoFloat48k(output)) ?
            S_OK : XAPO_E_FORMAT_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE IsOutputFormatSupported(const WAVEFORMATEX *input,
        const WAVEFORMATEX *output, WAVEFORMATEX **supported) override
    {
        if (supported) *supported = nullptr;
        return isStereoFloat48k(output) && (!input || isStereoFloat48k(input)) ?
            S_OK : XAPO_E_FORMAT_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE Initialize(const void *, UINT32) override
    {
        return S_OK;
    }

    void STDMETHODCALLTYPE Reset() override { xtalk = {}; }

    HRESULT STDMETHODCALLTYPE LockForProcess(UINT32 input_count,
        const XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS *input, UINT32 output_count,
        const XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS *output) override
    {
        if (input_count != 1 || output_count != 1 || !input || !output)
            return E_INVALIDARG;
        if (!isStereoFloat48k(input[0].pFormat) || !isStereoFloat48k(output[0].pFormat))
            return XAPO_E_FORMAT_UNSUPPORTED;
        if (input[0].MaxFrameCount != output[0].MaxFrameCount) return E_INVALIDARG;
        xtalk = {};
        return S_OK;
    }

    void STDMETHODCALLTYPE UnlockForProcess() override {}

    void STDMETHODCALLTYPE Process(UINT32 input_count,
        const XAPO_PROCESS_BUFFER_PARAMETERS *input, UINT32 output_count,
        XAPO_PROCESS_BUFFER_PARAMETERS *output, BOOL enabled) override
    {
        if (input_count != 1 || output_count != 1 || !input || !output) return;
        output[0].ValidFrameCount = input[0].ValidFrameCount;
        output[0].BufferFlags = input[0].BufferFlags;
        if (!enabled || !output[0].pBuffer || output[0].ValidFrameCount == 0) return;
        /* In place: silent input may leave the buffer undefined, but the
           canceller tail still rings, so zero it and filter as usual. */
        if (input[0].BufferFlags == XAPO_BUFFER_SILENT)
            std::memset(output[0].pBuffer, 0,
                static_cast<size_t>(output[0].ValidFrameCount) * 2u * sizeof(float));
        recomp_xtalk_process_block(&xtalk,
            static_cast<float *>(output[0].pBuffer), output[0].ValidFrameCount);
        output[0].BufferFlags = XAPO_BUFFER_VALID;
    }

    UINT32 STDMETHODCALLTYPE CalcInputFrames(UINT32 output_frames) override
    {
        return output_frames;
    }

    UINT32 STDMETHODCALLTYPE CalcOutputFrames(UINT32 input_frames) override
    {
        return input_frames;
    }

private:
    std::atomic<ULONG> references{1};
    RecompXtalk xtalk{};
};

} // namespace

IUnknown *recomp_audio_xtalk_xapo_create(void)
{
    return new (std::nothrow) Xtalk;
}
