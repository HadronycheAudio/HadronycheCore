#include "hadronyche/testhost/Host.h"

#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstattributes.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "public.sdk/source/common/memorystream.h"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#elif defined(__linux__)
#include <dlfcn.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <map>
#include <string>

using namespace Steinberg;

DEF_CLASS_IID (Vst::IEditController)
DEF_CLASS_IID (Vst::IComponent)
DEF_CLASS_IID (Vst::IAudioProcessor)
DEF_CLASS_IID (Vst::IConnectionPoint)
DEF_CLASS_IID (Vst::IMessage)
DEF_CLASS_IID (Vst::IAttributeList)
DEF_CLASS_IID (Vst::IHostApplication)
DEF_CLASS_IID (IPlugView)

namespace Hadronyche {
namespace TestHost {

namespace {

#define TESTS_NO_REFCOUNT                                                                     \
    tresult PLUGIN_API queryInterface (const TUID, void**) override { return kNoInterface; }  \
    uint32 PLUGIN_API addRef () override { return 1; }                                        \
    uint32 PLUGIN_API release () override { return 1; }

class Attributes : public Vst::IAttributeList
{
public:
    TESTS_NO_REFCOUNT

    tresult PLUGIN_API setInt (AttrID id, int64 value) override
    {
        _ints[id] = value;
        return kResultOk;
    }

    tresult PLUGIN_API getInt (AttrID id, int64& value) override
    {
        const auto it = _ints.find (id);
        if (it == _ints.end ())
            return kResultFalse;
        value = it->second;
        return kResultOk;
    }

    tresult PLUGIN_API setFloat (AttrID, double) override { return kResultFalse; }
    tresult PLUGIN_API getFloat (AttrID, double&) override { return kResultFalse; }
    tresult PLUGIN_API setString (AttrID, const Vst::TChar*) override { return kResultFalse; }
    tresult PLUGIN_API getString (AttrID, Vst::TChar*, uint32) override { return kResultFalse; }
    tresult PLUGIN_API setBinary (AttrID, const void*, uint32) override { return kResultFalse; }
    tresult PLUGIN_API getBinary (AttrID, const void*&, uint32&) override { return kResultFalse; }

private:
    std::map<std::string, int64> _ints;
};

class Message : public Vst::IMessage
{
public:
    tresult PLUGIN_API queryInterface (const TUID, void**) override { return kNoInterface; }
    uint32 PLUGIN_API addRef () override { return static_cast<uint32> (++_references); }

    uint32 PLUGIN_API release () override
    {
        const int left = --_references;
        if (left == 0)
            delete this;
        return static_cast<uint32> (left);
    }

    FIDString PLUGIN_API getMessageID () override { return _id.c_str (); }
    void PLUGIN_API setMessageID (FIDString id) override { _id = id; }
    Vst::IAttributeList* PLUGIN_API getAttributes () override { return &_attributes; }

private:
    int _references { 1 };
    std::string _id;
    Attributes _attributes;
};

// What a plug-in needs from a host: creating messages.
class Host : public Vst::IHostApplication
{
public:
    tresult PLUGIN_API queryInterface (const TUID iid, void** object) override
    {
        if (FUnknownPrivate::iidEqual (iid, Vst::IHostApplication::iid) || FUnknownPrivate::iidEqual (iid, FUnknown::iid))
        {
            *object = static_cast<Vst::IHostApplication*> (this);
            return kResultOk;
        }
        *object = nullptr;
        return kNoInterface;
    }

    uint32 PLUGIN_API addRef () override { return 1; }
    uint32 PLUGIN_API release () override { return 1; }

    tresult PLUGIN_API getName (Vst::String128 name) override
    {
        name[0] = 0;
        return kResultOk;
    }

    tresult PLUGIN_API createInstance (TUID classId, TUID, void** object) override
    {
        if (FUnknownPrivate::iidEqual (classId, Vst::IMessage::iid))
        {
            *object = new Message;
            return kResultOk;
        }
        *object = nullptr;
        return kResultFalse;
    }
};

class ValueQueue : public Vst::IParamValueQueue
{
public:
    TESTS_NO_REFCOUNT

    ValueQueue (Vst::ParamID id, double value) : _id (id), _value (value) {}

    Vst::ParamID PLUGIN_API getParameterId () override { return _id; }
    int32 PLUGIN_API getPointCount () override { return 1; }

    tresult PLUGIN_API getPoint (int32, int32& offset, Vst::ParamValue& value) override
    {
        offset = 0;
        value = _value;
        return kResultTrue;
    }

    tresult PLUGIN_API addPoint (int32, Vst::ParamValue, int32&) override { return kResultFalse; }

private:
    Vst::ParamID _id;
    double _value;
};

Host gHost;
#if defined(_WIN32)
HMODULE gModule = nullptr;
#elif defined(__APPLE__)
CFBundleRef gBundle = nullptr;
#elif defined(__linux__)
void* gLibrary = nullptr;
std::string gLibraryFile;
#endif
IPluginFactory* gFactory = nullptr;
bool (*gExitDll) () = nullptr;
TUID gComponentId;
TUID gControllerId;
int gFailures = 0;

} // namespace

class PlugBase::ParameterChanges : public Vst::IParameterChanges
{
public:
    TESTS_NO_REFCOUNT

    int32 PLUGIN_API getParameterCount () override { return static_cast<int32> (queues.size ()); }
    Vst::IParamValueQueue* PLUGIN_API getParameterData (int32 index) override { return &queues[static_cast<size_t> (index)]; }
    Vst::IParamValueQueue* PLUGIN_API addParameterData (const Vst::ParamID&, int32&) override { return nullptr; }

    std::vector<ValueQueue> queues;
};

namespace {

// Calls the plug-in's entry point and returns its factory function.
GetFactoryProc enterPlugin (const wchar_t* path)
{
#if defined(_WIN32)
    gModule = LoadLibraryW (path);
    if (!gModule)
        return nullptr;

    auto initDll = reinterpret_cast<bool (*) ()> (GetProcAddress (gModule, "InitDll"));
    gExitDll = reinterpret_cast<bool (*) ()> (GetProcAddress (gModule, "ExitDll"));
    auto getPluginFactory = reinterpret_cast<GetFactoryProc> (GetProcAddress (gModule, "GetPluginFactory"));
    return initDll && gExitDll && getPluginFactory && initDll () ? getPluginFactory : nullptr;
#elif defined(__APPLE__)
    // The path is to the binary, as CMake gives it: <name>.vst3/Contents/MacOS/<name>.
    std::string file (std::wcslen (path) * 4 + 1, '\0');
    file.resize (std::wcstombs (file.data (), path, file.size ()));
    for (int level = 0; level < 3; ++level)
        file.erase (file.rfind ('/'));

    CFURLRef url = CFURLCreateFromFileSystemRepresentation (nullptr, reinterpret_cast<const UInt8*> (file.data ()), static_cast<CFIndex> (file.size ()), true);
    gBundle = url ? CFBundleCreate (nullptr, url) : nullptr;
    if (url)
        CFRelease (url);
    if (!gBundle || !CFBundleLoadExecutable (gBundle))
        return nullptr;

    auto bundleEntry = reinterpret_cast<bool (*) (CFBundleRef)> (CFBundleGetFunctionPointerForName (gBundle, CFSTR ("bundleEntry")));
    gExitDll = reinterpret_cast<bool (*) ()> (CFBundleGetFunctionPointerForName (gBundle, CFSTR ("bundleExit")));
    auto getPluginFactory = reinterpret_cast<GetFactoryProc> (CFBundleGetFunctionPointerForName (gBundle, CFSTR ("GetPluginFactory")));
    return bundleEntry && gExitDll && getPluginFactory && bundleEntry (gBundle) ? getPluginFactory : nullptr;
#elif defined(__linux__)
    std::string file (std::wcslen (path) * 4 + 1, '\0');
    file.resize (std::wcstombs (file.data (), path, file.size ()));

    gLibraryFile = file;
    gLibrary = dlopen (file.c_str (), RTLD_LAZY | RTLD_LOCAL);
    if (!gLibrary)
    {
        // Nothing else would say which library or symbol is missing.
        std::printf ("%s\n", dlerror ());
        return nullptr;
    }

    // ModuleEntry keeps the handle, which VSTGUI uses to find the plug-in's bundle.
    auto moduleEntry = reinterpret_cast<bool (*) (void*)> (dlsym (gLibrary, "ModuleEntry"));
    gExitDll = reinterpret_cast<bool (*) ()> (dlsym (gLibrary, "ModuleExit"));
    auto getPluginFactory = reinterpret_cast<GetFactoryProc> (dlsym (gLibrary, "GetPluginFactory"));
    return moduleEntry && gExitDll && getPluginFactory && moduleEntry (gLibrary) ? getPluginFactory : nullptr;
#endif
}

} // namespace

bool loadPlugin (const wchar_t* path)
{
    const auto getPluginFactory = enterPlugin (path);
    if (!getPluginFactory)
        return false;

    gFactory = getPluginFactory ();
    for (int32 i = 0; i < gFactory->countClasses (); ++i)
    {
        PClassInfo info;
        gFactory->getClassInfo (i, &info);
        if (std::strcmp (info.category, kVstAudioEffectClass) == 0)
            std::memcpy (gComponentId, info.cid, sizeof (TUID));
        else if (std::strcmp (info.category, kVstComponentControllerClass) == 0)
            std::memcpy (gControllerId, info.cid, sizeof (TUID));
    }
    return true;
}

void unloadPlugin ()
{
    gFactory->release ();
    gExitDll ();
#if defined(_WIN32)
    FreeLibrary (gModule);
#elif defined(__APPLE__)
    CFRelease (gBundle);
#elif defined(__linux__)
    dlclose (gLibrary);
    // Pango's font thread outlives a panel, and crashed in the unloaded library when a test ended while it was busy.
    // The plug-in links with -z nodelete to stay loaded, as a host would need it to as well.
    void* stillLoaded = dlopen (gLibraryFile.c_str (), RTLD_LAZY | RTLD_LOCAL | RTLD_NOLOAD);
    check ("the plug-in stays loaded after dlclose (-z nodelete)", stillLoaded != nullptr);
    if (stillLoaded)
        dlclose (stillLoaded);
#endif
}

IPluginFactory* getFactory ()
{
    return gFactory;
}

void check (const char* what, bool ok)
{
    std::printf ("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++gFailures;
}

int getFailureCount ()
{
    return gFailures;
}

PlugBase::PlugBase (double sampleRate, int maxBlockSize) : _sampleRate (sampleRate), _changes (std::make_unique<ParameterChanges> ())
{
    gFactory->createInstance (gComponentId, Vst::IComponent::iid, reinterpret_cast<void**> (&component));
    component->initialize (&gHost);
    component->queryInterface (Vst::IAudioProcessor::iid, reinterpret_cast<void**> (&processor));

    gFactory->createInstance (gControllerId, Vst::IEditController::iid, reinterpret_cast<void**> (&controller));
    controller->initialize (&gHost);

    component->queryInterface (Vst::IConnectionPoint::iid, reinterpret_cast<void**> (&_componentConnection));
    controller->queryInterface (Vst::IConnectionPoint::iid, reinterpret_cast<void**> (&_controllerConnection));
    _componentConnection->connect (_controllerConnection);
    _controllerConnection->connect (_componentConnection);

    Vst::ProcessSetup setup { Vst::kRealtime, Vst::kSample32, maxBlockSize, sampleRate };
    processor->setupProcessing (setup);

    // An instrument has no audio input, and an effect has no event input.
    if (component->getBusCount (Vst::kAudio, Vst::kInput) > 0)
        component->activateBus (Vst::kAudio, Vst::kInput, 0, true);
    component->activateBus (Vst::kAudio, Vst::kOutput, 0, true);
    if (component->getBusCount (Vst::kEvent, Vst::kInput) > 0)
        component->activateBus (Vst::kEvent, Vst::kInput, 0, true);

    component->setActive (true);
    processor->setProcessing (true);
}

PlugBase::~PlugBase ()
{
    processor->setProcessing (false);
    component->setActive (false);
    _componentConnection->disconnect (_controllerConnection);
    _controllerConnection->disconnect (_componentConnection);
    _componentConnection->release ();
    _controllerConnection->release ();
    controller->terminate ();
    controller->release ();
    processor->release ();
    component->terminate ();
    component->release ();
}

void PlugBase::set (Vst::ParamID id, double plain)
{
    const double normalized = controller->plainParamToNormalized (id, plain);
    controller->setParamNormalized (id, normalized);
    _changes->queues.emplace_back (id, normalized);
}

uint64 PlugBase::runBlock (float** inputs, float** outputs, int count, Vst::IEventList* events)
{
    Vst::AudioBusBuffers inputBus {};
    inputBus.numChannels = 2;
    inputBus.channelBuffers32 = inputs;

    Vst::AudioBusBuffers outputBus {};
    outputBus.numChannels = 2;
    outputBus.channelBuffers32 = outputs;

    Vst::ProcessData data {};
    data.processMode = Vst::kRealtime;
    data.symbolicSampleSize = Vst::kSample32;
    data.numSamples = count;
    data.numInputs = inputs ? 1 : 0;
    data.inputs = inputs ? &inputBus : nullptr;
    data.numOutputs = 1;
    data.outputs = &outputBus;
    data.inputParameterChanges = _changes->queues.empty () ? nullptr : _changes.get ();
    data.inputEvents = events;
    data.processContext = context;

    processor->process (data);
    _changes->queues.clear ();

    return outputBus.silenceFlags;
}

Stereo PlugBase::process (const Samples& mono, int blockSize)
{
    return process (mono, mono, blockSize);
}

Stereo PlugBase::process (const Samples& left, const Samples& right, int blockSize)
{
    const size_t total = left.size ();

    Stereo result;
    result.left.resize (total);
    result.right.resize (total);

    Samples inputLeft = left;
    Samples inputRight = right;

    for (size_t position = 0; position < total; position += static_cast<size_t> (blockSize))
    {
        const int count = static_cast<int> (std::min<size_t> (static_cast<size_t> (blockSize), total - position));
        float* inputs[2] = { &inputLeft[position], &inputRight[position] };
        float* outputs[2] = { &result.left[position], &result.right[position] };

        result.blockSilent.push_back (runBlock (inputs, outputs, count) == 3);
    }
    return result;
}

std::vector<char> PlugBase::getStateBytes ()
{
    MemoryStream stream;
    component->getState (&stream);
    return std::vector<char> (stream.getData (), stream.getData () + stream.getSize ());
}

void PlugBase::setStateBytes (const std::vector<char>& bytes)
{
    MemoryStream forComponent;
    MemoryStream forController;
    int32 written = 0;
    int64 position = 0;
    forComponent.write (const_cast<char*> (bytes.data ()), static_cast<int32> (bytes.size ()), &written);
    forComponent.seek (0, IBStream::kIBSeekSet, &position);
    forController.write (const_cast<char*> (bytes.data ()), static_cast<int32> (bytes.size ()), &written);
    forController.seek (0, IBStream::kIBSeekSet, &position);
    component->setState (&forComponent);
    controller->setComponentState (&forController);
}

void PlugBase::sendToProcessor (const char* messageId, const char* attribute, int64 value)
{
    TUID messageClass;
    Vst::IMessage::iid.toTUID (messageClass);
    Vst::IMessage* message = nullptr;
    gHost.createInstance (messageClass, messageClass, reinterpret_cast<void**> (&message));
    message->setMessageID (messageId);
    message->getAttributes ()->setInt (attribute, value);
    _componentConnection->notify (message);
    message->release ();
}

} // namespace TestHost
} // namespace Hadronyche
