// This plugin is the interface between the decorator client and Wayfire. It has several functions:
//
// - When a new view is mapped, it notifies the decorator client via a custom protocol that a new decoration
//   is required.
// - When a new decoration toplevel is created, we attach it to the main view with several nodes:
//   First, we attach the translation node, which has a child mask node, whose child is the decoration surface.
//   The translation node is responsible for setting the position of the decoration relative to the main view.
//   The mask node cuts out the middle of the decoration so that transparent views remain transparent.
//   The main decoration surface contains the actual decorations.
//
// - On each transaction involving a decorated view, the plugin adds a decoration object associated with the
//   view to the transaction. The transaction object resizes the decoration on commit, and is ready when the
//   decoration surface also resizes to the new size. Special care should be taken for the cases where the
//   main view does not obey the compositor-requested size: in those cases, the decoration needs to be resized
//   again to the final size of the main view.

#include <map>
#include <iostream>
#include <linux/input-event-codes.h>
#include <memory>
#include <wayfire/core.hpp>
#include <wayfire/geometry.hpp>
#include <wayfire/nonstd/wlroots-full.hpp>
#include <wayfire/object.hpp>
#include <wayfire/plugin.hpp>
#include <wayfire/render-manager.hpp>
#include <wayfire/debug.hpp>
#include <wayfire/opengl.hpp>
#include <wayfire/scene-render.hpp>
#include <wayfire/scene.hpp>
#include <wayfire/toplevel-view.hpp>
#include <wayfire/scene-operations.hpp>
#include <wayfire/region.hpp>
#include <wayfire/matcher.hpp>

#include <wayfire/toplevel.hpp>
#include <wayfire/txn/transaction-object.hpp>
#include <wayfire/txn/transaction-manager.hpp>

#include <type_traits>
#include <wayfire/util.hpp>
#include <wayfire/view.hpp>
#include <wayfire/workarea.hpp>
#include <wayfire/window-manager.hpp>

#include <wayfire/signal-definitions.hpp>
#include "wf-decorator-protocol.h"

#include <wayfire/unstable/wlr-surface-node.hpp>
#include <wayfire/unstable/wlr-view-events.hpp>
#include <wayfire/unstable/translation-node.hpp>
#include "nonstd.hpp"

#define PRIV_COMMIT "_gtk3-deco-priv-commit"

static constexpr int margin_left = 31;
static constexpr int margin_top = 60;
static constexpr int margin_right = 31;
static constexpr int margin_bottom = 35;

static constexpr uint32_t STATE_FOCUSED   = 1;
static constexpr uint32_t STATE_MAXIMIZED = 2;
static constexpr uint32_t STATE_STICKY    = 4;
static constexpr uint32_t STATE_SHADED    = 8;

static int got_borders = 0;

static wf::decoration_margins_t deco_margins =
    {
        .left = 4,
        .right = 4,
        .bottom = 4,
        .top = 20,
};
static int borders_delta;

wl_resource *decorator_resource = NULL;
static void handle_activated_state();

std::ostream &operator<<(std::ostream &out, const wf::dimensions_t &dims)
{
    out << dims.width << "x" << dims.height;
    return out;
}

class extern_decoration_node_t : public wf::scene::wlr_surface_node_t //, public wf::pointer_interaction_t, public wf::touch_interaction_t
{
    public:
    int current_x, current_y, current_ms, may_be_hover = 0;
    uint32_t state = 0;
    wf::wl_timer<false> refresh_timer;
    bool shaded;
    std::shared_ptr<wf::scene::node_t> main_node;
    wf::geometry_t size;
    wf::geometry_t orig_size;
    
    wf::decoration_margins_t title_rect =
    {
        .left = 4,
        .right = 4,
        .bottom = 4,
        .top = 4,
    };
    int is_grabbed = 0;
    int view_id;
    std::weak_ptr<wf::toplevel_view_interface_t> _view;
    
    void handle_activated_state()
    {
    }
    extern_decoration_node_t (wlr_surface *v, bool b, wayfire_toplevel_view view) : wlr_surface_node_t(v,b)  //, node_t(false)
    {
        this->_view = view->weak_from_this();
        view_id = view->get_id();
        state = STATE_FOCUSED;
        if(view->pending_tiled_edges())
            state |= STATE_MAXIMIZED;
        if (decorator_resource)
            wf_decorator_manager_send_view_state_changed(decorator_resource, view_id, state);
        view->connect(&on_activated);
        view->connect(&on_tiled);
        view->connect(&sticky_changed);
    }

    ~extern_decoration_node_t ()
    {
        LOGI("extern_decoration_node_t deleted");
    }
    
    wf::signal::connection_t<wf::view_activated_state_signal> on_activated = [=] (auto)
    {
        if (decorator_resource)
        {
            handle_activated_state();
            state |= STATE_FOCUSED;
            wf_decorator_manager_send_view_state_changed(decorator_resource, view_id, state);
        }
    };
    
    wf::signal::connection_t<wf::view_tiled_signal> on_tiled = 
        [=] (wf::view_tiled_signal *ev)
    {
        LOGI("Maximized changed ", ev->old_edges," ", ev->new_edges);
        if (decorator_resource)
        {
            if (ev->new_edges == 0)
                state &= ~STATE_MAXIMIZED;
            else
                state |= STATE_MAXIMIZED;
            wf_decorator_manager_send_view_state_changed(decorator_resource, view_id, state);
            // this is needed because decoration is always a commit behind
            refresh_timer.set_timeout(100, [=] ()
            {
                wf_decorator_manager_send_reset_states(decorator_resource, view_id);
            });
        }          
    };
    
    wf::signal::connection_t<wf::view_set_sticky_signal> sticky_changed =
        [=](wf::view_set_sticky_signal *ev)
    {
        if (decorator_resource)
        {
            if (ev->view->sticky)
                state |= STATE_STICKY;
            else
                state &= ~STATE_STICKY;
            wf_decorator_manager_send_view_state_changed(decorator_resource, view_id, state);
            // this is needed because decoration is always a commit behind
            refresh_timer.set_timeout(100, [=] ()
            {
                wf_decorator_manager_send_reset_states(decorator_resource, view_id);
            });
        }          
    };
    
    
    wf::point_t get_offset() 
    {
        return { 0, 0 };
    }
    
    std::optional<wf::scene::input_node_t> find_node_at(const wf::pointf_t& at) override 
    {
        wf::pointf_t local = at - wf::pointf_t{get_offset()};
        return wf::scene::input_node_t{
            .node = this,
            .local_coords = local,
        };

        return {};
    }
/*
    pointer_interaction_t& pointer_interaction() override 
    {
        return *this;
    }

    void handle_pointer_enter(wf::pointf_t point) override  
    {
        current_x = point.x;
        current_y = point.y;
        update_cursor ();
    }

    void handle_pointer_leave() override
    {
        if (may_be_hover)
        {
            // send reset button states
            wf_decorator_manager_send_reset_states(decorator_resource, view_id);
            may_be_hover = 0;
        }
        update_cursor ();
    }

    void handle_pointer_motion(wf::pointf_t to, uint32_t time_ms) override
    {
        if (time_ms - current_ms > 100 && (current_x != (int)to.x || current_y != (int)to.y))
        {
            current_x = to.x;
            current_y = to.y;
            current_ms = time_ms;    
            uint32_t edges = update_cursor ();
            if (!edges)
            {
                if (to.x < title_rect.left || to.x > title_rect.right)
                {
                    may_be_hover = 1;
                    wf_decorator_manager_send_check_button(decorator_resource, view_id, current_x, current_y, is_grabbed);
                }                
                else
                {
                    if (may_be_hover)
                    {
                        // send reset button states
                        wf_decorator_manager_send_reset_states(decorator_resource, view_id);
                        may_be_hover = 0;
                    }
                }
            }
        }                                
    }

    void handle_pointer_button(const wlr_pointer_button_event& ev) override
    {
        if (ev.button != BTN_LEFT) {
            return;
        }
        is_grabbed = ev.state;
        uint32_t edge = calculate_resize_edges ();
        if (edge)
            handle_action (1, edge);
        else
        {
            if (current_x < title_rect.left || current_x > title_rect.right)
            {
                wf_decorator_manager_send_check_button(decorator_resource, view_id, current_x, current_y, is_grabbed);
                may_be_hover = 0;                
            }                
            else
                handle_action (0, 0);
        }
    }
    
    void handle_action (int action, uint32_t edge)
    {
        auto view = _view.lock();
        switch (action)
        {
            case 0:
                wf::get_core().default_wm->move_request(view);
                break;
            case 1:
                wf::get_core().default_wm->resize_request(view, edge);
                break;
            case 2:
                if (view->pending_tiled_edges()) {
                    wf::get_core().default_wm->tile_request(view, 0);
                } else {
                    wf::get_core().default_wm->tile_request(view, wf::TILED_EDGES_ALL);
                }
            default:  
                break;      
        }
    }        

    uint32_t calculate_resize_edges()
    {
        uint32_t edge = 0; 
        if (state & STATE_MAXIMIZED || state & STATE_SHADED)
            return edge;
        auto g = get_bounding_box();
        if ( current_x < deco_margins.left )
            edge |= WLR_EDGE_LEFT;
        if ( current_x > g.width - deco_margins.right)
            edge |= WLR_EDGE_RIGHT;
        if ( current_y > g.height - deco_margins.bottom)
            edge |= WLR_EDGE_BOTTOM;
        if ( current_y < title_rect.top)
            edge |= WLR_EDGE_TOP;
        return edge;
    }
    
    // Update the cursor based on @current_input
    uint32_t update_cursor() 
    {
        uint32_t edges = calculate_resize_edges();
        auto cursor_name = edges > 0 ?
            wlr_xcursor_get_resize_name((wlr_edges)edges) : "default";
        wf::get_core().set_cursor(cursor_name);
        return edges;
    }
*/
};  // extern_decoration_node_t

static std::map<uint32_t, std::shared_ptr<extern_decoration_node_t>> view_to_decor;


// reset all decorations activated state
static void handle_activated_state()
{
    for (const auto& pair : view_to_decor) 
    {
        pair.second->state &= ~STATE_FOCUSED;           
    }
}

/**
 * A node which cuts out a part of its children (visually).
 */
class extern_mask_node_t : public wf::scene::floating_inner_node_t
{
public:
    // The 'allowed' portion of the children
    wf::region_t allowed;
    int view_id;
    extern_mask_node_t() : floating_inner_node_t(false)
    {
    }
    ~extern_mask_node_t()
    {
        LOGI("extern_mask_node_t deleted");
        view_to_decor.erase(view_id);
        wf_decorator_manager_send_view_unmapped(decorator_resource, view_id);
        LOGI("view_to_decor ", view_to_decor.size());
    }
    std::optional<wf::scene::input_node_t> find_node_at(const wf::pointf_t &at) override
    {
        if (allowed.contains_pointf(at))
        {
            return wf::scene::floating_inner_node_t::find_node_at(at);
        }

        return {};
    }

    void gen_render_instances(std::vector<wf::scene::render_instance_uptr> &instances,
                              wf::scene::damage_callback push_damage, wf::output_t *output) override
    {
        instances.push_back(std::make_unique<gtk3_mask_render_instance_t>(this, push_damage, output));
    }

    class gtk3_mask_render_instance_t : public wf::scene::render_instance_t
    {
        std::vector<wf::scene::render_instance_uptr> children;
        wf::scene::damage_callback damage_cb;
        extern_mask_node_t *self;

        wf::signal::connection_t<wf::scene::node_damage_signal> on_self_damage =
            [=](wf::scene::node_damage_signal *ev)
        {
            damage_cb(ev->region);
        };

    public:
        gtk3_mask_render_instance_t(extern_mask_node_t *self, wf::scene::damage_callback damage_cb,
                                    wf::output_t *output)
        {
            this->self = self;
            this->damage_cb = damage_cb;
            for (auto &ch : self->get_children())
            {
                ch->gen_render_instances(children, damage_cb, output);
            }
        }

        void schedule_instructions(std::vector<wf::scene::render_instruction_t> &instructions,
                                   const wf::render_target_t &target, wf::region_t &damage) override
        {
            auto child_damage = damage & self->allowed;
            for (auto &ch : children)
            {
                ch->schedule_instructions(instructions, target, child_damage);
            }
        }

        void render(const wf::render_target_t &, const wf::region_t &) override
        {
            wf::dassert(false, "Rendering a gtk3_mask_node?");
        }

        void compute_visibility(wf::output_t *output, wf::region_t &visible) override
        {
            for (auto &ch : children)
            {
                ch->compute_visibility(output, visible);
            }
        }
    };
    
}; // extern_mask_node_t

class extern_decoration_object_t : public wf::txn::transaction_object_t
{
    enum class gtk3_decoration_tx_state
    {
        // No transactions in flight
        STABLE,
        // Transaction has just started
        START,
        // The decoration client has ACKed our initial size request. However, the decorated toplevel's client
        // has not ACKed the request yet, so we do not know the actual 'final' size of the client.
        TENTATIVE,
        // Decorated toplevel has set its final size, waiting for the decoration to respond.
        WAITING_FINAL,
    };
    
public:

    std::string stringify() const
    {
        std::ostringstream out;
        out << "gtk3deco(" << this << ")";
        return out.str();
    }

    void set_pending_size(wf::dimensions_t desired)
    {
        if (!toplevel)
        {
            return;
        }
        this->pending = desired;
    }

    void set_final_size(wf::dimensions_t final)
    {
        if (!toplevel)
        {
            return;
        }

        if (this->committed == final)
        {
            switch (this->deco_state)
            {
            case gtk3_decoration_tx_state::STABLE:
                return;

            case gtk3_decoration_tx_state::START:
                this->deco_state = gtk3_decoration_tx_state::WAITING_FINAL;
                break;

            case gtk3_decoration_tx_state::WAITING_FINAL:
                break;

            case gtk3_decoration_tx_state::TENTATIVE:
                // fallthrough
                this->deco_state = gtk3_decoration_tx_state::STABLE;
                wf::txn::emit_object_ready(this);
                break;
            }

            return;
        }

        this->committed = final;
        wlr_xdg_toplevel_set_size(toplevel, final.width, final.height);
        this->deco_state = gtk3_decoration_tx_state::WAITING_FINAL;
    }

    void size_updated()
    {
        wlr_box box;
        wlr_xdg_surface_get_geometry(toplevel->base, &box);

        if (wf::dimensions(box) != committed) 
        {
            return;
        }

        LOGI("Size is ", wf::dimensions(box), " state is ", (int)deco_state);

        switch (this->deco_state)
        {
        case gtk3_decoration_tx_state::STABLE:
            // Client simply committed, nothing has changed
            return;

        case gtk3_decoration_tx_state::TENTATIVE:
            // Client commits twice?
            return;

        case gtk3_decoration_tx_state::START:
            deco_state = gtk3_decoration_tx_state::TENTATIVE;
            break;

        case gtk3_decoration_tx_state::WAITING_FINAL:
            deco_state = gtk3_decoration_tx_state::STABLE;
            wf::txn::emit_object_ready(this);
            break;
        }
    }

    void commit()
    {
        if (!toplevel)
        {
            wf::txn::emit_object_ready(this);
            return;
        }
        auto dec_toplevel = decorated_toplevel.lock();

        set_pending_size(wf::dimensions(dec_toplevel->pending().geometry));
        deco_state = gtk3_decoration_tx_state::START;
        
        LOGI("Committing with ", pending);

        wlr_box box;
        wlr_xdg_surface_get_geometry(toplevel->base, &box);
        if (wf::dimensions(box) != pending)
        {
            wlr_xdg_toplevel_set_size(toplevel, pending.width, pending.height);
        }
        else
        {
            wf::txn::emit_object_ready(this);
            return;
        }

        committed = pending;
        size_updated();
    }

    void apply()
    {
        if (toplevel)
        {
            pending_state.merge_state(toplevel->base->surface);
        }
        auto tmp = deco_node.lock();
        tmp->apply_state(std::move(pending_state));
        recompute_mask();
    }

    std::weak_ptr<extern_decoration_node_t> deco_node;
    std::weak_ptr<wf::toplevel_t> decorated_toplevel;
    int first = 0;
    
    wf::wl_listener_wrapper on_request_move, on_request_resize, on_request_minimize, on_request_maximize,
        on_request_fullscreen;// on_show_window_menu;
    
    extern_decoration_object_t(
        wlr_xdg_toplevel *toplevel, std::weak_ptr<extern_decoration_node_t> deco_node,
        std::weak_ptr<extern_mask_node_t> mask, std::weak_ptr<wf::toplevel_t> decorated_toplevel)
    {
        this->toplevel = toplevel;
        this->deco_node = deco_node;
        this->mask_node = mask;
        auto tmp = mask.lock();
        auto node = deco_node.lock();
        tmp->view_id = node->view_id;
        this->decorated_toplevel = decorated_toplevel;
        
//        auto dec_toplevel = decorated_toplevel.lock();
//        auto decorated_view = wf::find_view_for_toplevel(dec_toplevel);
//        on_set_parent.connect(&toplevel->events.set_parent);
        on_request_move.connect(&toplevel->events.request_move);
//        on_request_resize.connect(&toplevel->events.request_resize);
//        on_request_maximize.connect(&toplevel->events.request_maximize);
//        on_request_minimize.connect(&toplevel->events.request_minimize);
////        on_show_window_menu.connect(&toplevel->events.request_show_window_menu);
//        on_request_fullscreen.connect(&toplevel->events.request_fullscreen);


        on_request_move.set_callback([&] (void*)
        {
            auto node = deco_node.lock();
            auto view = node->_view.lock();
            LOGI("on_request_move");
//            auto dec_toplevel = decorated_toplevel.lock();
            wf::get_core().default_wm->move_request(view);
        });
//        on_request_resize.set_callback([&] (auto data)
//        {
//            auto ev = static_cast<wlr_xdg_toplevel_resize_event*>(data);
//            wf::get_core().default_wm->resize_request({this}, ev->edges);
//        });
//        on_request_minimize.set_callback([&] (void*)
//        {
//            wf::get_core().default_wm->minimize_request({this}, true);
//        });
//        on_request_maximize.set_callback([&] (void *data)
//        {
//            wf::get_core().default_wm->tile_request({this},
//                toplevel->requested.maximized ? wf::TILED_EDGES_ALL : 0);
//        });
//        on_request_fullscreen.set_callback([&] (void *data)
//        {
//            wlr_xdg_toplevel_requested *req = &toplevel->requested;
//            auto wo = wf::get_core().output_layout->find_output(req->fullscreen_output);
//            wf::get_core().default_wm->fullscreen_request({this}, wo, req->fullscreen);
//        });

        on_commit.set_callback([=](void *)
                               {
            pending_state.merge_state(toplevel->base->surface);
            if (deco_state == gtk3_decoration_tx_state::STABLE)
            {
                auto tmp = deco_node.lock();
                tmp->apply_state(std::move(pending_state));
                recompute_mask();
            }
            size_updated(); });

        on_destroy.set_callback([=](void *)
                                {
            this->toplevel = nullptr;
            switch (deco_state)
            {
                case gtk3_decoration_tx_state::STABLE:
                  break;
                case gtk3_decoration_tx_state::START:
                  // fallthrough
                case gtk3_decoration_tx_state::TENTATIVE:
                  // fallthrough
                case gtk3_decoration_tx_state::WAITING_FINAL:
                  deco_state = gtk3_decoration_tx_state::STABLE;
                  wf::txn::emit_object_ready(this);
                  break;
            } });

        on_commit.connect(&toplevel->base->surface->events.commit);
        on_destroy.connect(&toplevel->base->events.destroy);
    }

    ~extern_decoration_object_t ()
    {
        LOGI("extern_decoration_object_t deleted");
    }
    
private:
    wf::dimensions_t pending = {0, 0};
    wf::dimensions_t committed = {0, 0};

    void recompute_mask()
    {
        auto masked = mask_node.lock();
        if(!masked)
        {
            return;
        }
        wf::dassert(masked != nullptr, "Masked node does not exist anymore??");
        auto tmp = deco_node.lock();
        auto bbox = tmp->get_bounding_box();

        // masked->allowed = wf::geometry_t{-100000, -10000, 10000000, 1000000};
        masked->allowed = bbox;
        wf::region_t cut_out = wf::geometry_t{
            .x = bbox.x + deco_margins.left,
            .y = bbox.y + deco_margins.top,
            .width = bbox.width - deco_margins.left - deco_margins.right,
            .height = bbox.height - deco_margins.top - deco_margins.bottom,
        };
        masked->allowed ^= cut_out;
    }

    wf::scene::surface_state_t pending_state;
    std::weak_ptr<extern_mask_node_t> mask_node;
    wf::wl_listener_wrapper on_commit, on_destroy;
    gtk3_decoration_tx_state deco_state = gtk3_decoration_tx_state::STABLE;

    wlr_xdg_toplevel *toplevel;

}; // extern_decoration_object_t


static bool begins_with(const std::string &a, const std::string &b)
{
    return a.substr(0, b.size()) == b;
}

static const std::string external_decorator_prefix = "__wf_decorator:";

void do_update_borders(wl_client *, struct wl_resource *, uint32_t top, uint32_t bottom, uint32_t left, uint32_t right, uint32_t delta)
{
    // give sane borders, with 0 px borders resize will be impossible.
    borders_delta = delta;
    deco_margins.left = left;
    deco_margins.right = right;
    deco_margins.bottom = bottom;
    deco_margins.top = top;
    LOGI("do_update_borders ", top, " ", bottom, " ", left, " ", right, " ", delta);
    got_borders = 1;
}

void do_update_title_rect(wl_client *, struct wl_resource *, uint32_t id, uint32_t top, uint32_t bottom, uint32_t left, uint32_t right)
{
    if (view_to_decor.count(id))
    {
        LOGI("do_update_title_rect");
        auto deco = view_to_decor[id];
        deco->title_rect.top = top;
        deco->title_rect.bottom = bottom; 
        deco->title_rect.left = left;
        deco->title_rect.right = right; 
        // this is needed because decoration is always a commit behind
        wf_decorator_manager_send_reset_states(decorator_resource, id);
    }
}

/* Button action sent by the client

Disclaimer: to achieve shade/unshade I resorted to a dirty trick with a caveat...
To shade, I remove from the toplevel the decorated view's node and re add it to unshade.
The caveat is: if a view is shaded, to maximize/unmaximize it I unshade before proceeding
because if not UGLY things will happen...
Also, a shaded view is not resizable.
Maybe there is a more elegant and robust way to achieve this with transactions and/or 
additional nodes, even with animation.
But I'm pretty new to wayfire, up to now my knowledge reaches so far...

*/
 
void do_window_action(wl_client *, struct wl_resource *, uint32_t id, const char *action)
{
    LOGI("action ", action);
    wayfire_toplevel_view view;
    for (auto &v : wf::get_core().get_all_views())
    {
        if (v->get_id() == id)
        {
            view = toplevel_cast(v);
            break;
        }
    }
    if (strcmp (action, "minimize") == 0)
    {
        wf::get_core().default_wm->minimize_request(view, true);
    }
    else if (strcmp (action, "maximize") == 0)
    {
        if (view_to_decor.count(id))
        {
            auto deco = view_to_decor[id];
            // if the toplevel is shaded unshade it
            if (deco->state & STATE_SHADED)
            {
                do_window_action(NULL, NULL, id, "unshade");
            }
        }
        if (view->pending_tiled_edges()) {
            wf::get_core().default_wm->tile_request(view, 0);
        } else {
            wf::get_core().default_wm->tile_request(view, wf::TILED_EDGES_ALL);
        }
    }
    else if (strcmp (action, "close") == 0)
    {
        view->close();
    }
    else if (strcmp (action, "stick") == 0)
    {
        view->set_sticky(1);
    }
    else if (strcmp (action, "unstick") == 0)
    {
        view->set_sticky(0);
    }
    else if (strcmp (action, "shade") == 0)
    {
        if (view_to_decor.count(id))
        {
            auto deco = view_to_decor[id];
            deco->state |= STATE_SHADED;
            // dirty trick... to shade remove the decorated view node, keeping it for unshade later
            deco->main_node = view->get_surface_root_node()->get_children()[0];
            wf::scene::remove_child(deco->main_node, 0);
            // maybe not needed, but...
            wf::scene::set_node_enabled(deco->main_node, false);
            wf::get_core().tx_manager->schedule_object(view->toplevel());
            wf_decorator_manager_send_view_state_changed(decorator_resource, id, deco->state);
        }            
    }
    else if (strcmp (action, "unshade") == 0)
    {
        if (view_to_decor.count(id))
        {
            auto deco = view_to_decor[id];
            deco->state &= ~STATE_SHADED;
            // readd decorated view node
            wf::scene::add_front(view->get_surface_root_node(), deco->main_node);
            wf::scene::set_node_enabled(deco->main_node, true);
            wf::get_core().tx_manager->schedule_object(view->toplevel());
            wf_decorator_manager_send_view_state_changed(decorator_resource, id, deco->state);
        }            
    }
}

// protocol interface
const struct wf_decorator_manager_interface decorator_implementation =
    {
        .update_borders = do_update_borders,
        .window_action = do_window_action,
        .update_title_rect = do_update_title_rect
    };

// never called
void unbind_decorator(wl_resource *decorator_resource)
{
    wl_resource_destroy (decorator_resource);
    decorator_resource = NULL;
}

void bind_decorator(wl_client *client, void *, uint32_t, uint32_t id)
{
    LOGI("Binding wf-external-decorator");
    auto resource = wl_resource_create(client, &wf_decorator_manager_interface, 1, id);
    wl_resource_set_implementation(resource, &decorator_implementation, NULL, NULL);
    decorator_resource = resource;
}

class extern_toplevel_custom_data : public wf::custom_data_t
{
public:
    std::shared_ptr<extern_decoration_object_t> decoration;
    std::shared_ptr<wf::scene::translation_node_t> translation_node;
    ~extern_toplevel_custom_data()
    {
        LOGI("extern_toplevel_custom_data deleted");
    }
};

class external_decoration_plugin : public wf::plugin_interface_t
{
    wf::view_matcher_t ignore_views{"wf-external-decorator/ignore_views"};

    wl_global *decorator_global;
    int running = 0;
    bool first_run = true;
    wf::wl_timer<true> timer;
    
    wf::signal::connection_t<wf::new_xdg_surface_signal> on_new_xdg_surface =
        [=](wf::new_xdg_surface_signal *ev)
    {
        if (!decorator_resource)
            return;

        if (ev->surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL)
        {
            return;
        }

        auto toplevel = ev->surface->toplevel;
        
        if (!begins_with(nonull(toplevel->title), external_decorator_prefix))
        {
            return;
        }

        LOGI("Got decorator view ", toplevel->title);
        auto id_str = std::string(toplevel->title).substr(external_decorator_prefix.length());
        auto id = std::stoul(id_str.c_str());

        wayfire_toplevel_view target;
        for (auto &v : wf::get_core().get_all_views())
        {
            if (v->get_id() == id)
            {
                target = toplevel_cast(v);
                break;
            }
        }

        if (!target)
        {
            LOGI("View is gone already?");
            wlr_xdg_toplevel_send_close(toplevel);
            return;
        }

        if (!target->toplevel())
        {
            LOGI("View does not support toplevel interface?");
            wlr_xdg_toplevel_send_close(toplevel);
            return;
        }

        int maximized = target->pending_tiled_edges();

        // update title
        wf_decorator_manager_send_title_changed(decorator_resource, target->get_id(), target->get_title().c_str());

        ev->use_default_implementation = false;
        
        auto data = target->toplevel()->get_data_safe<extern_toplevel_custom_data>();
        auto decoration_root_node = std::make_shared<wf::scene::translation_node_t>();
        int extra = maximized ? borders_delta : 0;
        decoration_root_node->set_offset({-(deco_margins.left - extra) , -(deco_margins.top - extra)});
        data->translation_node = decoration_root_node;
        auto mask_node = std::make_shared<extern_mask_node_t>();
        decoration_root_node->set_children_list({mask_node});

        auto deco_surf = std::make_shared<extern_decoration_node_t>(toplevel->base->surface, true, target);
        
        view_to_decor[target->get_id()] = deco_surf;
        
        data->decoration = std::make_shared<extern_decoration_object_t>(
            toplevel, deco_surf, mask_node, target->toplevel());
        mask_node->set_children_list({deco_surf});
        wf::scene::add_back(target->get_surface_root_node(), decoration_root_node);
        
        target->toplevel()->connect(&on_object_ready);
        // Trigger a new transaction to set margins
        wf::get_core().tx_manager->schedule_object(target->toplevel());
    };

    wf::signal::connection_t<wf::view_title_changed_signal> title_set =
        [=](wf::view_title_changed_signal *ev)
    {
        if (decorator_resource)
        {
            LOGI("Title changed ", ev->view->get_title());
            wf_decorator_manager_send_title_changed(decorator_resource, ev->view->get_id(), ev->view->get_title().c_str());
        }
    };

    wf::signal::connection_t<wf::view_mapped_signal> on_mapped = [=](wf::view_mapped_signal *ev)
    {
        if (!running)
            return;
        auto v = toplevel_cast(ev->view);
        if (v)
        {
            // check if the view is not already decorated and should be decorated
            // apps like smplayer with systray enabled remap the toplevel when you click the systray icon
            // to re show  
            if (!view_to_decor.count(v->get_id()) && v->should_be_decorated() && !ignore_decoration_of_view(v))
            {
                auto toplevel = wf::toplevel_cast(ev->view);    
                // if parent is nullptr is a main window, if not is a dialog             
                send_create_decoration(ev->view, toplevel->parent != nullptr);
            }
        }        
    };

    wf::signal::connection_t<wf::txn::new_transaction_signal> on_new_tx = [=](wf::txn::new_transaction_signal *ev)
    {
        auto objs = ev->tx->get_objects();
        for (auto &obj : objs)
        {
            if (auto toplevel = std::dynamic_pointer_cast<wf::toplevel_t>(obj))
            {
                // First check whether the toplevel already has decoration
                // In that case, we should just set the correct margins
                if (auto deco = toplevel->get_data<extern_toplevel_custom_data>())
                {
                    auto& pending = toplevel->pending();
                    int delta = pending.tiled_edges ? 0 : borders_delta;
                    wf::decoration_margins_t margins;
                    margins.top = deco_margins.top - delta;
                    margins.left = deco_margins.left - delta;
                    margins.right = deco_margins.right - delta;
                    margins.bottom = deco_margins.bottom - delta;
                    
                    // adjust offset
                    deco->translation_node->set_offset({-margins.left, -margins.top});
                    
                    pending.margins = toplevel->pending().fullscreen ? wf::decoration_margins_t{0, 0, 0, 0} : margins;
                                        
                    if (deco->decoration->first == 0)
                    {
                        auto view = wf::find_view_for_toplevel(toplevel);
                        if (wlr_surface_is_xwayland_surface(view->get_wlr_surface()))
                        {
                            // this is needed for xwayland views, dunno why
                            pending.geometry = wf::expand_geometry_by_margins(pending.geometry, pending.margins);
                        }
                        deco->decoration->first = 1;
                    }
                    ev->tx->add_object(deco->decoration);
                }
            }
        }
    };

    wf::signal::connection_t<wf::txn::object_ready_signal> on_object_ready = [=](wf::txn::object_ready_signal *ev)
    {
        auto toplvl = dynamic_cast<wf::toplevel_t *>(ev->self);
        auto deco = toplvl->get_data<extern_toplevel_custom_data>();
        wf::dassert(deco != nullptr, "obj ready for non-decorated toplevel??");
        LOGI("on_object_ready");
        deco->decoration->set_final_size(wf::dimensions(toplvl->committed().geometry));
    };

    void send_create_decoration(wayfire_view view, bool type)
    {
        const char *app_id = view->get_app_id().c_str();
        if (decorator_resource && app_id && strcmp(app_id,"nil"))
        {
            if(!view->get_wlr_surface())
                return;                    
            LOGI("Need decoration for ", view);
            view->connect(&title_set);
            wf_decorator_manager_send_create_new_decoration(decorator_resource, view->get_id(), type);
        }
    }

    void remove_decoration(wayfire_toplevel_view view)
    {
        auto target = toplevel_cast(view);
        if (view_to_decor.count(target->get_id()))
        {
            auto deco_node = view_to_decor[target->get_id()];
            view_to_decor.erase(target->get_id());
        }
        auto data = target->toplevel()->release_data<extern_toplevel_custom_data>();
        if (data)
        {
            LOGI("Decoration removed ", view->get_title());
            // this is needed to properly free all nodes
            auto cl = target->get_surface_root_node()->get_children();
            auto tl = cl[1];
            wf::scene::remove_child(tl, 0);
            // tell the client to free resources
            wf_decorator_manager_send_view_unmapped(decorator_resource, target->get_id());
            LOGI("view_to_decor ", view_to_decor.size());
        }
    }

    bool ignore_decoration_of_view(wayfire_view view) 
    {
        return ignore_views.matches(view);
    }
    
    void decorate_present_views ()
    {
        if (decorator_resource)
        {
            for (auto &view : wf::get_core().get_all_views())
            {
                auto toplevel = toplevel_cast(view);
                if (toplevel && toplevel->should_be_decorated()  && !ignore_decoration_of_view(toplevel))
                {
                    send_create_decoration(view, toplevel->parent != nullptr);
                }                    
            }
        }
    }
    void setup ()
    {
        decorate_present_views ();
        wf::get_core().connect(&on_mapped);
        wf::get_core().connect(&on_new_xdg_surface);
        wf::get_core().tx_manager->connect(&on_new_tx);
    }
          
public:
    wf::option_wrapper_t<std::string> decorator{"wf-external-decorator/decorator"};          
    pid_t decorator_pid;
    
    void init() override
    {
        LOGI("start external_decoration_plugin");
        
        running = 1;
        // spawn the configured client executable
        decorator_pid = wf::get_core().run((std::string)decorator);                                              

        if(first_run)
        {
            // only bind the protocol the first time
            decorator_global = wl_global_create(wf::get_core().display,
                                                &wf_decorator_manager_interface,
                                                1, NULL, bind_decorator);
            first_run = false;
        }
            
        timer.set_timeout(20, [=] ()
        {
            // wait for client borders request
            if (got_borders)
            {
                LOGI("got_borders");
                setup ();
                return false;
            }
            return true;
        });
            
    }
    
    void fini() override
    {
        LOGI("stop external_decoration_plugin");
        running = 0;
        got_borders = 0;
        for (auto view : wf::get_core().get_all_views())
        {
            if (auto toplevel = wf::toplevel_cast(view))
            {
                remove_decoration(toplevel);
            }
        }
        // kill the decoration client
        kill (decorator_pid, SIGKILL);
    }
    
};

DECLARE_WAYFIRE_PLUGIN(external_decoration_plugin);
