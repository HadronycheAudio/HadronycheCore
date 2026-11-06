#include "hadronyche/testhost/Host.h"
#include "hadronyche/testhost/Panel.h"
#include "hadronyche/testhost/Runner.h"

// That the headers stand by themselves, and that a plug-in's tests can add to PlugBase.
class MinimalPlug : public Hadronyche::TestHost::PlugBase
{
public:
    void press () { sendToProcessor ("message", "attribute", 1); }
};

int checkRunner (int argc, wchar_t** argv)
{
    return Hadronyche::TestHost::runTests (argc, argv, { "MinimalTests", nullptr, 0, nullptr });
}
