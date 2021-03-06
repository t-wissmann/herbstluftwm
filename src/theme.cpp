#include "theme.h"

using std::vector;
using std::string;

Theme::Theme() {
    // add sub-decorations array as children
    vector<string> type_names = {
        "fullscreen",
        "tiling",
        "floating",
        "minimal",
    };
    for (int i = 0; i < (int)Type::Count; i++) {
        addStaticChild(&dec[i], type_names[i]);
        dec[i].triple_changed_.connect([this](){ this->theme_changed_.emit(); });
    }

    // forward attribute changes: only to tiling and floating
    auto &t = dec[(int)Type::Tiling], &f = dec[(int)Type::Floating];
    active.makeProxyFor({&t.active, &f.active});
    normal.makeProxyFor({&t.normal, &f.normal});
    urgent.makeProxyFor({&t.urgent, &f.urgent});
}

DecorationScheme::DecorationScheme()
    : reset(this, "reset", &DecorationScheme::resetGetterHelper,
                           &DecorationScheme::resetSetterHelper)
    , proxyAttributes_ ({
        &border_width,
        &border_color,
        &tight_decoration,
        &inner_color,
        &inner_width,
        &outer_color,
        &outer_width,
        &padding_top,
        &padding_right,
        &padding_bottom,
        &padding_left,
        &background_color,
    })
{
    for (auto i : proxyAttributes_) {
        addAttribute(i->toAttribute());
        i->toAttribute()->setWriteable();
        i->toAttribute()->changed().connect([this]() { this->scheme_changed_.emit(); });
    }
}

DecTriple::DecTriple()
{
    addStaticChild(&normal, "normal");
    addStaticChild(&active, "active");
    addStaticChild(&urgent, "urgent");
    vector<DecorationScheme*> children = {
        &normal,
        &active,
        &urgent,
    };
    makeProxyFor(children);
    for (auto it : children) {
        it->scheme_changed_.connect([this]() { this->triple_changed_.emit(); });
    }
}

//! reset all attributes to a default value
string DecorationScheme::resetSetterHelper(string)
{
    for (auto it : attributes()) {
        it.second->resetValue();
    }
    return {};
}

string DecorationScheme::resetGetterHelper() {
    return "Writing this resets all attributes to a default value";
}

void DecorationScheme::makeProxyFor(vector<DecorationScheme*> decs) {
    for (auto it : proxyAttributes_) {
        for (auto target : decs) {
            it->addProxyTarget(target);
        }
    }
}


