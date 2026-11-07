#pragma once

#include "pluginterfaces/vst/ivstmessage.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Hadronyche {
namespace Lv2 {

// A VST3 message that can be turned into bytes and back, which the LV2 wrapper carries between a processor and its
// controller through atom ports. The SDK's own message keeps its attributes where nothing can list them.
class Message : public Steinberg::Vst::IMessage, public Steinberg::Vst::IAttributeList
{
public:
    // Starts with one reference, which the caller releases.
    static Message* create ();

    // Null if the bytes aren't a whole message. They come from a host, so nothing in them is trusted.
    static Message* fromBytes (const void* bytes, size_t size);

    std::vector<uint8_t> toBytes () const;

    // IMessage
    Steinberg::FIDString PLUGIN_API getMessageID () override { return _id.c_str (); }
    void PLUGIN_API setMessageID (Steinberg::FIDString id) override { _id = id ? id : ""; }
    Steinberg::Vst::IAttributeList* PLUGIN_API getAttributes () override { return this; }

    // IAttributeList
    Steinberg::tresult PLUGIN_API setInt (AttrID id, Steinberg::int64 value) override;
    Steinberg::tresult PLUGIN_API getInt (AttrID id, Steinberg::int64& value) override;
    Steinberg::tresult PLUGIN_API setFloat (AttrID id, double value) override;
    Steinberg::tresult PLUGIN_API getFloat (AttrID id, double& value) override;
    Steinberg::tresult PLUGIN_API setString (AttrID id, const Steinberg::Vst::TChar* string) override;
    Steinberg::tresult PLUGIN_API getString (AttrID id, Steinberg::Vst::TChar* string, Steinberg::uint32 sizeInBytes) override;
    Steinberg::tresult PLUGIN_API setBinary (AttrID id, const void* data, Steinberg::uint32 sizeInBytes) override;
    Steinberg::tresult PLUGIN_API getBinary (AttrID id, const void*& data, Steinberg::uint32& sizeInBytes) override;

    // FUnknown
    Steinberg::tresult PLUGIN_API queryInterface (const Steinberg::TUID iid, void** object) override;
    Steinberg::uint32 PLUGIN_API addRef () override;
    Steinberg::uint32 PLUGIN_API release () override;

private:
    enum class Type : uint8_t
    {
        Int,
        Float,
        String,
        Binary,
    };

    struct Value
    {
        Type type { Type::Int };
        Steinberg::int64 integer { 0 };
        double number { 0.0 };
        std::vector<uint8_t> bytes; // A string's UTF-16 units without the terminator, or the binary.
    };

    Message () = default;
    virtual ~Message () = default;

    const Value* find (AttrID id, Type type) const;

    std::atomic<Steinberg::uint32> _references { 1 };
    std::string _id;
    std::map<std::string, Value> _values;
};

} // namespace Lv2
} // namespace Hadronyche
