#include "client.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "clientmanager.h"
#include "decoration.h"
#include "ewmh.h"
#include "globals.h"
#include "hook.h"
#include "ipc-protocol.h"
#include "keymanager.h"
#include "layout.h"
#include "monitor.h"
#include "mousemanager.h"
#include "root.h"
#include "settings.h"
#include "stack.h"
#include "tag.h"
#include "theme.h"
#include "utils.h"
#include "xconnection.h"

using std::string;

static int g_monitor_float_treshold = 24;

static Client* lastfocus = nullptr;


Client::Client(Window window, bool visible_already, ClientManager& cm)
    : window_(window)
    , dec(make_unique<Decoration>(this, *cm.settings))
    , visible_(visible_already)
    , urgent_(this, "urgent", false)
    , fullscreen_(this,  "fullscreen", false)
    , title_(this,  "title", "")
    , tag_str_(this,  "tag", &Client::tagName)
    , window_id_str(this,  "winid", "")
    , keyMask_(this,  "keymask", "")
    , pid_(this,  "pid", -1)
    , pseudotile_(this,  "pseudotile", false)
    , ewmhrequests_(this, "ewmhrequests", true)
    , ewmhnotify_(this, "ewmhnotify", true)
    , sizehints_floating_(this, "sizehints", true)
    , sizehints_tiling_(this, "sizehints", false)
    , manager(cm)
    , theme(*cm.theme)
    , settings(*cm.settings)
    , ewmh(*cm.ewmh)
{
    std::stringstream tmp;
    window_id_str = WindowID(window).str();
    keyMask_.setWriteable();
    ewmhnotify_.setWriteable();
    for (auto i : {&fullscreen_, &pseudotile_}) {
        i->setWriteable();
        i->changed().connect([this](bool){ needsRelayout.emit(this->tag()); });
    }

    keyMask_.changed().connect([this] {
            if (Root::get()->clients()->focus() == this) {
                Root::get()->keys()->ensureKeyMask();
            }
            });
    fullscreen_.changed().connect(this, &Client::updateEwmhState);

    init_from_X();
}

void Client::init_from_X() {
    // treat wanted coordinates as floating coords
    float_size_ = Root::get()->X.windowSize(window_);
    last_size_ = float_size_;

    pid_ = Root::get()->X.windowPid(window_);

    update_title();
    update_wm_hints();
    updatesizehints();
}

void Client::make_full_client() {
    // setup decoration
    XSetWindowBorderWidth(g_display, window_, 0);
    // specify that the client window survives if hlwm dies, i.e. it will be
    // reparented back to root
    XChangeSaveSet(g_display, window_, SetModeInsert);
    XReparentWindow(g_display, window_, dec->decorationWindow(), 40, 40);
    // if this client is visible, then reparenting will make it invisible
    // and will create a unmap notify event
    if (visible_ == true) {
        ignore_unmaps_++;
        visible_ = false;
    }
    // get events from window
    XSelectInput(g_display, dec->decorationWindow(), (EnterWindowMask | LeaveWindowMask |
                            ButtonPressMask | ButtonReleaseMask |
                            ExposureMask |
                            SubstructureRedirectMask | FocusChangeMask));
    XSelectInput(g_display, window_,
                            StructureNotifyMask|FocusChangeMask
                            |EnterWindowMask|PropertyChangeMask);
}

bool Client::ignore_unmapnotify() {
    if (ignore_unmaps_ > 0) {
        ignore_unmaps_--;
        return true;
    } else {
        return false;
    }
}

void reset_client_colors() {
    all_monitors_apply_layout();
}

Client* get_client_from_window(Window window) {
    return Root::get()->clients()->client(window);
}

// destroys a special client
Client::~Client() {
    if (lastfocus == this) {
        lastfocus = nullptr;
    }
    if (slice) {
        delete slice;
    }
}

bool Client::needs_minimal_dec() {
    //if (!frame) {
    //    frame = this->tag()->frame->frameWithClient(this);
    //    HSAssert(frame != nullptr);
    //}
    if (this->pseudotile_()) return false;
    if (this->is_client_floated()) return false;
    //if (smart_window_surroundings_active(frame)) return true;
    return false;
}

void Client::window_unfocus() {
    Root::get()->mouse->grab_client_buttons(this, false);
}

void Client::window_unfocus_last() {
    if (lastfocus) {
        lastfocus->window_unfocus();
    }
    // give focus to root window
    Ewmh::get().clearInputFocus();
    if (lastfocus) {
        /* only emit the hook if the focus *really* changes */
        hook_emit({"focus_changed", "0x0", ""});
        Ewmh::get().updateActiveWindow(None);
        tag_update_each_focus_layer();

        // Enable all keys in the root window
        Root::get()->keys()->clearActiveKeyMask();
    }
    lastfocus = 0;
}

void Client::window_focus() {
    // set keyboard focus
    if (!this->neverfocus_) {
        XSetInputFocus(g_display, this->window_, RevertToPointerRoot, CurrentTime);
    }
    else ewmh.sendEvent(window_, Ewmh::WM::TakeFocus, True);

    if (this != lastfocus) {
        /* FIXME: this is a workaround because window_focus always is called
         * twice.  see BUGS for more information
         *
         * only emit the hook if the focus *really* changes */
        // unfocus last one
        if (lastfocus) {
            lastfocus->window_unfocus();
        }
        ewmh.updateActiveWindow(this->window_);
        tag_update_each_focus_layer();
        hook_emit({"focus_changed", WindowID(window_).str(), title_()});
    }

    // change window-colors
    //HSDebug("window_focus ACTIVE: 0x%lx\n", client->window);
    //client_setup_border(client, true);

    lastfocus = this;
    /* do some specials for the max layout */
    bool is_max_layout = HSFrame::getGloballyFocusedFrame()->focusedClient() == this
                         && HSFrame::getGloballyFocusedFrame()->getLayout() == LayoutAlgorithm::max
                         && get_current_monitor()->tag->floating == false;
    if (settings.raise_on_focus() || is_max_layout) {
        this->raise();
    }
    tag_update_focus_layer(get_current_monitor()->tag);
    Root::get()->mouse->grab_client_buttons(this, true);

    // XXX: At this point, ClientManager does not yet know about the focus
    // change. So as a workaround, we pass ourselves directly to KeyManager:
    Root::get()->keys()->ensureKeyMask(this);

    this->set_urgent(false);
}

const DecTriple& Client::getDecTriple() {
    auto triple_idx = Theme::Type::Tiling;
    if (fullscreen_()) triple_idx = Theme::Type::Fullscreen;
    else if (is_client_floated()) triple_idx = Theme::Type::Floating;
    else if (needs_minimal_dec()) triple_idx = Theme::Type::Minimal;
    else triple_idx = Theme::Type::Tiling;
    return theme[triple_idx];
}

void Client::setup_border(bool focused) {
    dec->change_scheme(getDecTriple()(focused, urgent_()));
}

void Client::resize_fullscreen(Rectangle monitor_rect, bool isFocused) {
    dec->resize_outline(monitor_rect, theme[Theme::Type::Fullscreen](isFocused,urgent_()));
}

void Client::raise() {
    this->tag()->stack->raiseSlice(this->slice);
}

void Client::resize_tiling(Rectangle rect, bool isFocused) {
    // apply border width
    if (!this->pseudotile_ /* && !smart_window_surroundings_active(frame) */) {
        // apply window gap
        rect.width -= settings.window_gap();
        rect.height -= settings.window_gap();
    }
    auto& scheme = theme[Theme::Type::Tiling](isFocused, urgent_());
    if (this->pseudotile_) {
        auto inner = this->float_size_;
        applysizehints(&inner.width, &inner.height);
        auto outline = scheme.inner_rect_to_outline(inner);
        rect.x += std::max(0, (rect.width - outline.width)/2);
        rect.y += std::max(0, (rect.height - outline.height)/2);
        rect.width = std::min(outline.width, rect.width);
        rect.height = std::min(outline.height, rect.height);
    }
    dec->resize_outline(rect, scheme);
}

// from dwm.c
bool Client::applysizehints(int *w, int *h) {
    bool baseismin;

    /* set minimum possible */
    *w = std::max(1, *w);
    *h = std::max(1, *h);
    if(*h < WINDOW_MIN_HEIGHT)
        *h = WINDOW_MIN_HEIGHT;
    if(*w < WINDOW_MIN_WIDTH)
        *w = WINDOW_MIN_WIDTH;
    bool sizehints = (this->is_client_floated() || this->pseudotile_)
                        ? this->sizehints_floating_
                        : this->sizehints_tiling_;
    if (sizehints) {
        /* see last two sentences in ICCCM 4.1.2.3 */
        baseismin = this->basew_ == this->minw_ && this->baseh_ == this->minh_;
        if(!baseismin) { /* temporarily remove base dimensions */
            *w -= this->basew_;
            *h -= this->baseh_;
        }
        /* adjust for aspect limits */
        if(this->mina_ > 0 && this->maxa_ > 0) {
            if(this->maxa_ < (float)*w / *h)
                *w = *h * int(this->maxa_ + 0.5f);
            else if(this->mina_ < (float)*h / *w)
                *h = *w * int(this->mina_ + 0.5f);
        }
        if(baseismin) { /* increment calculation requires this */
            *w -= this->basew_;
            *h -= this->baseh_;
        }
        /* adjust for increment value */
        if(this->incw_)
            *w -= *w % this->incw_;
        if(this->inch_)
            *h -= *h % this->inch_;
        /* restore base dimensions */
        *w += this->basew_;
        *h += this->baseh_;
        *w = std::max(*w, this->minw_);
        *h = std::max(*h, this->minh_);
        if(this->maxw_)
            *w = std::min(*w, this->maxw_);
        if(this->maxh_)
            *h = std::min(*h, this->maxh_);
    }
    return *w != this->last_size_.width || *h != this->last_size_.height;
}

bool Client::applysizehints_xy(int *x, int *y,
                                 int *w, int *h) {
    return applysizehints(w,h) || *x != this->last_size_.x
                               || *y != this->last_size_.y;
}

// from dwm.c
void Client::updatesizehints() {
    long msize;
    XSizeHints size;

    if(!XGetWMNormalHints(g_display, this->window_, &size, &msize))
        /* size is uninitialized, ensure that size.flags aren't used */
        size.flags = PSize;
    if(size.flags & PBaseSize) {
        this->basew_ = size.base_width;
        this->baseh_ = size.base_height;
    }
    else if(size.flags & PMinSize) {
        this->basew_ = size.min_width;
        this->baseh_ = size.min_height;
    } else {
        this->basew_ = this->baseh_ = 0;
    }
    if(size.flags & PResizeInc) {
        this->incw_ = size.width_inc;
        this->inch_ = size.height_inc;
    }
    else
        this->incw_ = this->inch_ = 0;
    if(size.flags & PMaxSize) {
        this->maxw_ = size.max_width;
        this->maxh_ = size.max_height;
    } else {
        this->maxw_ = this->maxh_ = 0;
    }
    if(size.flags & PMinSize) {
        this->minw_ = size.min_width;
        this->minh_ = size.min_height;
    }
    else if(size.flags & PBaseSize) {
        this->minw_ = size.base_width;
        this->minh_ = size.base_height;
    } else {
        this->minw_ = this->minh_ = 0;
    }
    if(size.flags & PAspect) {
        this->mina_ = (float)size.min_aspect.y / size.min_aspect.x;
        this->maxa_ = (float)size.max_aspect.x / size.max_aspect.y;
    } else {
        this->maxa_ = this->mina_ = 0.0;
    }
    //this->isfixed = (this->maxw && this->minw && this->maxh && this->minh
    //             && this->maxw == this->minw && this->maxh == this->minh);
}




void Client::send_configure() {
    auto last_inner_rect = dec->last_inner();
    XConfigureEvent ce;
    ce.type = ConfigureNotify;
    ce.display = g_display;
    ce.event = this->window_;
    ce.window = this->window_;
    ce.x = last_inner_rect.x;
    ce.y = last_inner_rect.y;
    ce.width = std::max(last_inner_rect.width, WINDOW_MIN_WIDTH);
    ce.height = std::max(last_inner_rect.height, WINDOW_MIN_HEIGHT);
    ce.border_width = 0;
    ce.above = None;
    ce.override_redirect = False;
    XSendEvent(g_display, this->window_, False, StructureNotifyMask, (XEvent *)&ce);
}

void Client::resize_floating(Monitor* m, bool isFocused) {
    if (!m) return;
    auto rect = this->float_size_;
    rect.x += m->rect.x;
    rect.x += m->rect.y;
    rect.x += m->pad_left();
    rect.y += m->pad_up();
    // ensure position is on monitor
    int space = g_monitor_float_treshold;
    rect.x =
        CLAMP(rect.x,
              m->rect.x + m->pad_left() - rect.width + space,
              m->rect.x + m->rect.width - m->pad_left() - m->pad_right() - space);
    rect.y =
        CLAMP(rect.y,
              m->rect.y + m->pad_up() - rect.height + space,
              m->rect.y + m->rect.height - m->pad_up() - m->pad_down() - space);
    dec->resize_inner(rect, theme[Theme::Type::Floating](isFocused,urgent_()));
}

Rectangle Client::outer_floating_rect() {
    return dec->inner_to_outer(float_size_);
}

int close_command(Input input, Output) {
    string winid = "";
    input >> winid; // try to read, use "" otherwise
    auto window = get_window(winid);
    if (window != 0)
        Ewmh::get().windowClose(window);
    else return HERBST_INVALID_ARGUMENT;
    return 0;
}

bool Client::is_client_floated() {
    auto t = tag();
    if (!t) return false;
    else return tag()->floating;
}

void Client::requestClose() { //! ask the client to close
    ewmh.windowClose(window_);
}

void Client::set_visible(bool visible) {
    if (visible == this->visible_) return;
    if (visible) {
        /* Grab the server to make sure that the frame window is mapped before
           the client gets its MapNotify, i.e. to make sure the client is
           _visible_ when it gets MapNotify. */
        XGrabServer(g_display);
        ewmh.windowUpdateWmState(this->window_, WmStateNormalState);
        XMapWindow(g_display, this->window_);
        XMapWindow(g_display, this->dec->decorationWindow());
        XUngrabServer(g_display);
    } else {
        /* we unmap the client itself so that we can get MapRequest
           events, and because the ICCCM tells us to! */
        XUnmapWindow(g_display, this->dec->decorationWindow());
        XUnmapWindow(g_display, this->window_);
        ewmh.windowUpdateWmState(this->window_, WmStateWithdrawnState);
        this->ignore_unmaps_++;
    }
    this->visible_ = visible;
}

// heavily inspired by dwm.c
void Client::set_urgent(bool state) {
    if (this->urgent_() == state) {
        // nothing to do
        return;
    }
    set_urgent_force(state);
}

void Client::set_urgent_force(bool state) {
    hook_emit({"urgent", state ? "on" : "off", WindowID(window_).str() });

    this->urgent_ = state;

    setup_border(this == HSFrame::getGloballyFocusedFrame()->focusedClient());

    XWMHints *wmh;
    if(!(wmh = XGetWMHints(g_display, this->window_)))
        return;

    if (state) {
        wmh->flags |= XUrgencyHint;
    } else {
        wmh->flags &= ~XUrgencyHint;
    }

    XSetWMHints(g_display, this->window_, wmh);
    XFree(wmh);
    // report changes to tags
    tag_set_flags_dirty();
}

// heavily inspired by dwm.c
void Client::update_wm_hints() {
    XWMHints* wmh = XGetWMHints(g_display, this->window_);
    if (!wmh) {
        return;
    }

    Client* focused_client = HSFrame::getGloballyFocusedFrame()->focusedClient();
    if ((focused_client == this)
        && wmh->flags & XUrgencyHint) {
        // remove urgency hint if window is focused
        wmh->flags &= ~XUrgencyHint;
        XSetWMHints(g_display, this->window_, wmh);
    } else {
        bool newval = (wmh->flags & XUrgencyHint) ? true : false;
        if (newval != this->urgent_()) {
            this->urgent_ = newval;
            this->setup_border(focused_client == this);
            hook_emit({"urgent", urgent_() ? "on":"off", WindowID(window_).str()});
            tag_set_flags_dirty();
        }
    }
    if (wmh->flags & InputHint) {
        this->neverfocus_ = !wmh->input;
    } else {
        this->neverfocus_ = false;
    }
    XFree(wmh);
}

void Client::update_title() {
    string newName = ewmh.getWindowTitle(window_);
    bool changed = title_() != newName;
    title_ = newName;
    if (changed && get_current_client() == this) {
        hook_emit({"window_title_changed", WindowID(window_).str(), title_()});
    }
}

Client* get_current_client() {
    return HSFrame::getGloballyFocusedFrame()->focusedClient();
}

void Client::set_fullscreen(bool state) {
    // TODO: move all these things to appropriate places:
    //  - Slice management should not be done by the client
    //  - fullscreen-hook should be done by the ClientManager
    //  - Monitr::applyLayout should be called via hooks
    if (fullscreen_() == state) return;
    fullscreen_ = state;
    if (this->ewmhnotify_) {
        this->ewmhfullscreen_ = state;
    }
    auto stack = this->tag()->stack;
    if (state) {
        stack->sliceAddLayer(this->slice, LAYER_FULLSCREEN);
    } else {
        stack->sliceRemoveLayer( this->slice, LAYER_FULLSCREEN);
    }
    tag_update_focus_layer(this->tag());
    auto m = find_monitor_with_tag(this->tag());
    if (m) m->applyLayout();

    ewmh.updateWindowState(this);
    hook_emit({"fullscreen", state ? "on" : "off", WindowID(window_).str()});
}

void Client::updateEwmhState() {
    if (ewmhnotify_) {
        ewmhfullscreen_ = fullscreen_();
    }
    ewmh.updateWindowState(this);
}

void Client::set_pseudotile(bool state) {
    this->pseudotile_ = state;
    auto m = find_monitor_with_tag(this->tag());
    if (m) m->applyLayout();
}

/**
 * \brief   Resolve a window description to a client
 *
 * \param   str     Describes the window: "" means the focused one, "urgent"
 *                  resolves to a arbitrary urgent window, "0x..." just
 *                  resolves to the given window given its hexadecimal window id,
 *                  a decimal number its decimal window id.
 * \return          Pointer to the resolved client, or null, if client not found
 */
Client* get_client(const char* str) {
    if (!strcmp(str, "")) {
        return get_current_client();
    } else {
        return Root::get()->clients()->client(str);
    }
}

/**
 * \brief   Resolve a window description to a window
 *
 * \param   str     Describes the window: "" means the focused one, "urgent"
 *                  resolves to a arbitrary urgent window, "0x..." just
 *                  resolves to the given window given its hexadecimal window id,
 *                  a decimal number its decimal window id.
 * \return          Window id, or 0, if unconvertable
 */
Window get_window(const string& str) {
    // managed window?
    auto client = get_client(str.c_str());
    if (client)
        return client->window_;

    // unmanaged window? try to convert from base 16 or base 10 at the same time
    try {
        return Converter<WindowID>::parse(str);
    } catch (...) {
        return 0;
    }
}

void Client::fuzzy_fix_initial_position() {
    // find out the top-left-most position of the decoration,
    // considering the current settings of possible floating decorations
    int extreme_x = float_size_.x;
    int extreme_y = float_size_.y;
    const auto& t = theme[Theme::Type::Floating];
    auto r = t.active.inner_rect_to_outline(float_size_);
    extreme_x = std::min(extreme_x, r.x);
    extreme_y = std::min(extreme_y, r.y);
    r = t.normal.inner_rect_to_outline(float_size_);
    extreme_x = std::min(extreme_x, r.x);
    extreme_y = std::min(extreme_y, r.y);
    r = t.urgent.inner_rect_to_outline(float_size_);
    extreme_x = std::min(extreme_x, r.x);
    extreme_y = std::min(extreme_y, r.y);
    // if top left corner might be outside of the monitor, move it accordingly
    if (extreme_x < 0) { float_size_.x += abs(extreme_x); }
    if (extreme_y < 0) { float_size_.y += abs(extreme_y); }
}

void Client::clear_properties() {
    ewmh.clearClientProperties(window_);
}

//! name of the tag on which the client is
string Client::tagName() {
    // be safe during initialization phase and don't assume
    // that tag is set.
    return tag_ ? tag_->name() : "";
}

Window Client::decorationWindow() {
    return dec->decorationWindow();
}

