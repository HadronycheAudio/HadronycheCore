#pragma once

#include "hadronyche/lv2/PluginInfo.h"

#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/urid/urid.h>

#include <cstdint>
#include <cstring>
#include <string>

namespace Hadronyche {
namespace Lv2 {

// How a message's bytes travel in the message ports: an atom:Object of the type <plug-in URI#message>, whose one
// property <plug-in URI#bytes> is an atom:Chunk. Hosts pass objects between a UI and its plug-in, and this keeps the
// bytes, from Message::toBytes, opaque to them. Writing and reading neither allocate nor lock.
class MessageAtoms
{
public:
    void map (LV2_URID_Map* map, const PluginInfo& info)
    {
        _object = map->map (map->handle, LV2_ATOM__Object);
        _chunk = map->map (map->handle, LV2_ATOM__Chunk);
        _messageType = map->map (map->handle, (std::string (info.uri) + "#message").c_str ());
        _bytesKey = map->map (map->handle, (std::string (info.uri) + "#bytes").c_str ());
    }

    // The atom's size with its header, for size bytes.
    static uint32_t atomSize (uint32_t size)
    {
        return static_cast<uint32_t> (sizeof (LV2_Atom_Object) + sizeof (LV2_Atom_Property_Body)) + size;
    }

    // Writes the atom to where there's room for atomSize (size).
    void write (void* destination, const void* bytes, uint32_t size) const
    {
        auto* object = static_cast<LV2_Atom_Object*> (destination);
        object->atom.size = atomSize (size) - static_cast<uint32_t> (sizeof (LV2_Atom));
        object->atom.type = _object;
        object->body.id = 0;
        object->body.otype = _messageType;

        auto* property = reinterpret_cast<LV2_Atom_Property_Body*> (object + 1);
        property->key = _bytesKey;
        property->context = 0;
        property->value.size = size;
        property->value.type = _chunk;
        std::memcpy (property + 1, bytes, size);
    }

    // False for anything that isn't a message, whole and inside the size given.
    bool read (const LV2_Atom* atom, uint32_t available, const void*& bytes, uint32_t& size) const
    {
        if (available < sizeof (LV2_Atom) || atom->size > available - sizeof (LV2_Atom) || atom->type != _object
            || atom->size < sizeof (LV2_Atom_Object_Body))
            return false;

        const auto* object = reinterpret_cast<const LV2_Atom_Object*> (atom);
        if (object->body.otype != _messageType)
            return false;

        LV2_ATOM_OBJECT_FOREACH (object, property)
        {
            const uint32_t end = static_cast<uint32_t> (reinterpret_cast<const uint8_t*> (property + 1) - reinterpret_cast<const uint8_t*> (&object->body));
            if (end > object->atom.size || property->value.size > object->atom.size - end)
                return false;
            if (property->key == _bytesKey && property->value.type == _chunk)
            {
                bytes = property + 1;
                size = property->value.size;
                return true;
            }
        }
        return false;
    }

private:
    LV2_URID _object { 0 };
    LV2_URID _chunk { 0 };
    LV2_URID _messageType { 0 };
    LV2_URID _bytesKey { 0 };
};

} // namespace Lv2
} // namespace Hadronyche
