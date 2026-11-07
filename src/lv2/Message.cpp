#include "hadronyche/lv2/Message.h"

#include <algorithm>
#include <cstring>

using namespace Steinberg;

namespace Hadronyche {
namespace Lv2 {

namespace {

// Larger than anything a plug-in sends, and small enough that a damaged length can't ask for much memory.
constexpr uint32_t kMaxPart = 1 << 20;

void putUint (std::vector<uint8_t>& out, uint64_t value, int size)
{
    for (int i = 0; i < size; ++i)
        out.push_back (static_cast<uint8_t> (value >> (8 * i)));
}

void putPart (std::vector<uint8_t>& out, const void* data, size_t size)
{
    putUint (out, size, 4);
    const auto* bytes = static_cast<const uint8_t*> (data);
    out.insert (out.end (), bytes, bytes + size);
}

// Reads the format toBytes writes, little-endian, and fails on anything short or too large.
class Reader
{
public:
    Reader (const void* bytes, size_t size) : _bytes (static_cast<const uint8_t*> (bytes)), _left (size) {}

    bool getUint (uint64_t& value, int size)
    {
        if (_left < static_cast<size_t> (size))
            return false;
        value = 0;
        for (int i = 0; i < size; ++i)
            value |= static_cast<uint64_t> (_bytes[i]) << (8 * i);
        _bytes += size;
        _left -= static_cast<size_t> (size);
        return true;
    }

    bool getPart (const uint8_t*& data, uint32_t& size)
    {
        uint64_t length = 0;
        if (!getUint (length, 4) || length > kMaxPart || length > _left)
            return false;
        data = _bytes;
        size = static_cast<uint32_t> (length);
        _bytes += length;
        _left -= length;
        return true;
    }

    bool isAtEnd () const { return _left == 0; }

private:
    const uint8_t* _bytes;
    size_t _left;
};

} // namespace

Message* Message::create ()
{
    return new Message;
}

std::vector<uint8_t> Message::toBytes () const
{
    std::vector<uint8_t> out;
    putPart (out, _id.data (), _id.size ());
    putUint (out, _values.size (), 4);
    for (const auto& [key, value] : _values)
    {
        putPart (out, key.data (), key.size ());
        putUint (out, static_cast<uint8_t> (value.type), 1);
        if (value.type == Type::Int)
            putUint (out, static_cast<uint64_t> (value.integer), 8);
        else if (value.type == Type::Float)
        {
            uint64_t bits = 0;
            std::memcpy (&bits, &value.number, sizeof (bits));
            putUint (out, bits, 8);
        }
        else
            putPart (out, value.bytes.data (), value.bytes.size ());
    }
    return out;
}

Message* Message::fromBytes (const void* bytes, size_t size)
{
    Reader reader (bytes, size);
    const uint8_t* part = nullptr;
    uint32_t partSize = 0;
    uint64_t count = 0;
    if (!reader.getPart (part, partSize) || !reader.getUint (count, 4) || count > kMaxPart)
        return nullptr;

    auto* message = new Message;
    message->_id.assign (reinterpret_cast<const char*> (part), partSize);

    bool ok = true;
    for (uint64_t i = 0; ok && i < count; ++i)
    {
        uint64_t type = 0;
        ok = reader.getPart (part, partSize) && reader.getUint (type, 1) && type <= static_cast<uint8_t> (Type::Binary);
        if (!ok)
            break;

        const std::string key (reinterpret_cast<const char*> (part), partSize);
        Value value;
        value.type = static_cast<Type> (type);
        uint64_t bits = 0;
        if (value.type == Type::Int)
        {
            ok = reader.getUint (bits, 8);
            value.integer = static_cast<int64> (bits);
        }
        else if (value.type == Type::Float)
        {
            ok = reader.getUint (bits, 8);
            std::memcpy (&value.number, &bits, sizeof (bits));
        }
        else
        {
            ok = reader.getPart (part, partSize) && (value.type != Type::String || partSize % sizeof (Vst::TChar) == 0);
            if (ok)
                value.bytes.assign (part, part + partSize);
        }
        if (ok)
            message->_values[key] = std::move (value);
    }

    if (!ok || !reader.isAtEnd ())
    {
        message->release ();
        return nullptr;
    }
    return message;
}

const Message::Value* Message::find (AttrID id, Type type) const
{
    if (!id)
        return nullptr;
    const auto found = _values.find (id);
    return found != _values.end () && found->second.type == type ? &found->second : nullptr;
}

tresult PLUGIN_API Message::setInt (AttrID id, int64 value)
{
    if (!id)
        return kInvalidArgument;
    Value& entry = _values[id] = Value {};
    entry.type = Type::Int;
    entry.integer = value;
    return kResultOk;
}

tresult PLUGIN_API Message::getInt (AttrID id, int64& value)
{
    const Value* entry = find (id, Type::Int);
    if (!entry)
        return kResultFalse;
    value = entry->integer;
    return kResultOk;
}

tresult PLUGIN_API Message::setFloat (AttrID id, double value)
{
    if (!id)
        return kInvalidArgument;
    Value& entry = _values[id] = Value {};
    entry.type = Type::Float;
    entry.number = value;
    return kResultOk;
}

tresult PLUGIN_API Message::getFloat (AttrID id, double& value)
{
    const Value* entry = find (id, Type::Float);
    if (!entry)
        return kResultFalse;
    value = entry->number;
    return kResultOk;
}

tresult PLUGIN_API Message::setString (AttrID id, const Vst::TChar* string)
{
    if (!id || !string)
        return kInvalidArgument;
    size_t length = 0;
    while (string[length] != 0)
        ++length;
    Value& entry = _values[id] = Value {};
    entry.type = Type::String;
    const auto* bytes = reinterpret_cast<const uint8_t*> (string);
    entry.bytes.assign (bytes, bytes + length * sizeof (Vst::TChar));
    return kResultOk;
}

tresult PLUGIN_API Message::getString (AttrID id, Vst::TChar* string, uint32 sizeInBytes)
{
    const Value* entry = find (id, Type::String);
    if (!entry || !string || sizeInBytes < sizeof (Vst::TChar))
        return kResultFalse;
    const size_t units = std::min<size_t> (entry->bytes.size () / sizeof (Vst::TChar), sizeInBytes / sizeof (Vst::TChar) - 1);
    std::memcpy (string, entry->bytes.data (), units * sizeof (Vst::TChar));
    string[units] = 0;
    return kResultOk;
}

tresult PLUGIN_API Message::setBinary (AttrID id, const void* data, uint32 sizeInBytes)
{
    if (!id || (!data && sizeInBytes > 0))
        return kInvalidArgument;
    Value& entry = _values[id] = Value {};
    entry.type = Type::Binary;
    const auto* bytes = static_cast<const uint8_t*> (data);
    entry.bytes.assign (bytes, bytes + sizeInBytes);
    return kResultOk;
}

tresult PLUGIN_API Message::getBinary (AttrID id, const void*& data, uint32& sizeInBytes)
{
    const Value* entry = find (id, Type::Binary);
    if (!entry)
        return kResultFalse;
    data = entry->bytes.data ();
    sizeInBytes = static_cast<uint32> (entry->bytes.size ());
    return kResultOk;
}

tresult PLUGIN_API Message::queryInterface (const TUID iid, void** object)
{
    if (FUnknownPrivate::iidEqual (iid, Vst::IMessage::iid) || FUnknownPrivate::iidEqual (iid, FUnknown::iid))
    {
        addRef ();
        *object = static_cast<Vst::IMessage*> (this);
        return kResultOk;
    }
    if (FUnknownPrivate::iidEqual (iid, Vst::IAttributeList::iid))
    {
        addRef ();
        *object = static_cast<Vst::IAttributeList*> (this);
        return kResultOk;
    }
    *object = nullptr;
    return kNoInterface;
}

uint32 PLUGIN_API Message::addRef ()
{
    return ++_references;
}

uint32 PLUGIN_API Message::release ()
{
    const uint32 left = --_references;
    if (left == 0)
        delete this;
    return left;
}

} // namespace Lv2
} // namespace Hadronyche
