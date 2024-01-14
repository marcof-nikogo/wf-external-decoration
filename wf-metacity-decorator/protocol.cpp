#include "protocol.hpp"
#include "wf-decorator-client-protocol.h"
#include <wayland-client.h>
#include <string.h>
#include <iostream>
#include <map>

wl_display *display;
wf_decorator_manager *decorator_manager;

static std::map<uint32_t, GtkWidget*> view_to_decor;
static std::map<GtkWidget*, uint32_t> decor_to_view;

static void create_new_decoration(void*, wf_decorator_manager*, uint32_t view, uint32_t type)
{
    std::cout << "create new decoration" << std::endl;
    GtkWidget *window = create_deco_window("__wf_decorator:" + std::to_string(view), type);
    
    view_to_decor[view] = window;
    decor_to_view[window] = view;
}

static void title_changed(void*,
    wf_decorator_manager*, uint32_t view, const char *new_title)
{
    if(view_to_decor.count(view) > 0)
    {
        std::cout << "title_changed" << std::endl;
        set_title(view_to_decor[view], new_title);
    }        
}

static void reset_states(void*, wf_decorator_manager*, uint32_t view)
{
    if(view_to_decor.count(view) > 0)
    {
        std::cout << "reset_states" << std::endl;
        reset_deco_states(view_to_decor[view]);
    }

}
static void check_button(void*,
    wf_decorator_manager*, uint32_t view, uint32_t x, uint32_t y, uint32_t pressed)
{
    if(view_to_decor.count(view) > 0)
    {
        //std::cout << "check_button" << std::endl;
        update_buttons(view_to_decor[view], x, y, pressed);
    }        
}

static void view_state_changed(void*,
    wf_decorator_manager*, uint32_t view, uint32_t state)
{
    if(view_to_decor.count(view) > 0)
    {
        std::cout << "view state changed " << state << std::endl;
        set_view_state(view_to_decor[view], state);
    }        
}

static void view_unmapped(void*,
    wf_decorator_manager*, uint32_t view)
{
    if(view_to_decor.count(view) > 0)
    {
        std::cout << "view_unmapped" << std::endl;
        GtkWidget *window = view_to_decor[view];
        set_view_unmapped(window);
        view_to_decor.erase(view);
        decor_to_view.erase(window);
    }        
}

void update_borders(uint32_t top, uint32_t bottom, uint32_t left, uint32_t right, uint32_t delta)
{
    wf_decorator_manager_update_borders(decorator_manager, top, bottom, left, right, delta);
}

void window_action(GtkWidget *window, const char *action)
{
    wf_decorator_manager_window_action(decorator_manager, decor_to_view[window], action);
}

void update_title_rect(GtkWidget *window, uint32_t top, uint32_t bottom, uint32_t left, uint32_t right)
{
    wf_decorator_manager_update_title_rect(decorator_manager, decor_to_view[window], 
                                       top, bottom, left, right);
}

const wf_decorator_manager_listener decorator_listener =
{
    create_new_decoration,
    title_changed,
    check_button,
    view_state_changed,
    reset_states,
    view_unmapped
};

void registry_add_object(void*, struct wl_registry *registry, uint32_t name,
        const char *interface, uint32_t)
{
    // std::cout << "new registry: " << interface << std::endl;
    if (strcmp(interface, wf_decorator_manager_interface.name) == 0)
    {
        std::cout << "bind it" << std::endl;
        decorator_manager =
            (wf_decorator_manager*) wl_registry_bind(registry, name, &wf_decorator_manager_interface, 1u);

        wf_decorator_manager_add_listener(decorator_manager, &decorator_listener, NULL);
    }
}

void registry_remove_object(void*, struct wl_registry*, uint32_t)
{
}

static struct wl_registry_listener registry_listener =
{
    &registry_add_object,
    &registry_remove_object
};


void setup_protocol(GdkDisplay *displ)
{
    auto display = gdk_wayland_display_get_wl_display(displ);
    auto registry = wl_display_get_registry(display);

    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    wl_registry_destroy(registry);
}
