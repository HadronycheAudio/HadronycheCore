// For a plug-in that gives hadronyche_add_lv2 no LINKS, which passes messages on unchanged, if it has message ports.

#include "hadronyche/lv2/MessageLink.h"

std::unique_ptr<Hadronyche::Lv2::DspLink> Hadronyche::Lv2::createDspLink ()
{
    return nullptr;
}

std::unique_ptr<Hadronyche::Lv2::UiLink> Hadronyche::Lv2::createUiLink ()
{
    return nullptr;
}
