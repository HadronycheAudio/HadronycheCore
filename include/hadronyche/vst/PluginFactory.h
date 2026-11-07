#pragma once

// The VST3 module entry of a plug-in with one processor and one controller, from its PluginConfig.h. Only the
// plug-in's PluginEntry.cpp includes this, because the SDK's factory lives in whichever file does.
//
//   HADRONYCHE_PLUGIN_FACTORY (MyPlugin, MYPLUGIN, Vst::PlugType::kFx, Vst::kDistributable)
//
// PREFIX names the macros MYPLUGIN_NAME, _VENDOR, _URL, _EMAIL and _VERSION, and the plug-in's Processor and
// Controller have getClassFUID and createInstance.

#include "public.sdk/source/main/pluginfactory.h"

#define HADRONYCHE_PLUGIN_FACTORY(Namespace, PREFIX, subCategories, classFlags)                   \
    BEGIN_FACTORY_DEF (PREFIX##_VENDOR, PREFIX##_URL, PREFIX##_EMAIL)                             \
    DEF_CLASS2 (INLINE_UID_FROM_FUID (Namespace::Processor::getClassFUID ()),                     \
                PClassInfo::kManyInstances, kVstAudioEffectClass, PREFIX##_NAME, classFlags,      \
                subCategories, PREFIX##_VERSION, kVstVersionString, Namespace::Processor::createInstance) \
    DEF_CLASS2 (INLINE_UID_FROM_FUID (Namespace::Controller::getClassFUID ()),                    \
                PClassInfo::kManyInstances, kVstComponentControllerClass, PREFIX##_NAME, 0, "",   \
                PREFIX##_VERSION, kVstVersionString, Namespace::Controller::createInstance)       \
    END_FACTORY
