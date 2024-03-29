/*

This is a decoration client based on gtk3 and the rendering engine of marco, the windows manager
of Mate Desktop, used also by the latest versions of compiz.
It parses and renders metacity themes of all versions, i.e. v1, v2 and v3. 
Not all metacity themes out there support the stick and shade buttons, only those made for marco. 

The view_state_changed event carries a state, a bit mask:

static uint32_t STATE_FOCUSED   = 1;
static uint32_t STATE_MAXIMIZED = 2;
static uint32_t STATE_STICKY    = 4;
static uint32_t STATE_SHADED    = 8;

*/
#include <map>
#include <iostream>
#include <fstream>
#include <nlohmann/json.hpp>
#include "protocol.hpp"
#include "nonstd.hpp"

using json = nlohmann::json;

static json config;

GtkApplication *app;
MetaTheme * metatheme;
MetaFrameGeometry fgeom;
GtkWidget *view_focused;
PangoFontDescription *font_desc = pango_font_description_from_string("Bitstream Vera Sans Book 11");
MetaButtonLayout        button_layout, dialog_button_layout;

#define MODE_HOVER   0
#define MODE_CLICK   1
#define MODE_RELEASE 2

class decoration_data_t
{
public:

    MetaFrameGeometry       frame_geometry;
    MetaButtonState         button_states[META_BUTTON_TYPE_LAST];
    PangoLayout             *layout = NULL;
    int                     text_height;
    char                    *title = NULL;
    int                     last_hover_state = 0;
    MetaButtonFunction      last_active_button = META_BUTTON_FUNCTION_LAST;
    MetaButtonFunction      last_pressed_button = META_BUTTON_FUNCTION_LAST;
    int                     state = 0;
    uint                    type = 0;
    GdkRectangle            *title_bar;
    int                     current_edge = -1;
    
    ~decoration_data_t ()
    {
        if (title)
            g_free (title);
    }
    
    decoration_data_t (GtkWidget *window, uint what)
    {
        type = what;            // 0 toplevel, 1 dialog
        reset_button_states ();
        title_bar = &frame_geometry.title_rect;
        create_title_layout (window);
    }
    
    void create_title_layout(GtkWidget *window)
    {
        layout = gtk_widget_create_pango_layout(window, title ? title : "  ");
        pango_layout_set_font_description(layout, font_desc);  
        pango_layout_set_wrap (layout, PANGO_WRAP_CHAR);
        pango_layout_set_auto_dir (layout, FALSE);
        pango_layout_get_pixel_size (layout, NULL, &text_height);
    }    

    void reset_button_states ()
    {
        for (int i = 0; i < META_BUTTON_TYPE_LAST; i++)
            button_states[i] = META_BUTTON_STATE_NORMAL;
        last_active_button = last_pressed_button = META_BUTTON_FUNCTION_LAST;
        last_hover_state = 0;
    }

    // check if the pointer is on a button and draw it accordingly 
    gboolean check_button (int mode, int x, int y, MetaButtonState state, int pressed, MetaButtonFunction *what)
    {
        *what = META_BUTTON_FUNCTION_LAST;
        if (y > title_bar->y + title_bar->height)
            return FALSE;
            
        MetaButtonFunction *which_buttons;
        if (mode == MODE_RELEASE)
        {
            if (last_hover_state && last_active_button != META_BUTTON_FUNCTION_LAST)
            {
                button_states[meta_function_to_type(last_active_button)] = META_BUTTON_STATE_PRELIGHT;
                *what = last_active_button;
                last_active_button = META_BUTTON_FUNCTION_LAST;
                last_pressed_button = META_BUTTON_FUNCTION_LAST;
                last_hover_state = 0;
                return TRUE;
            }
        }
        // choose left or right buttons depending on the position respect the title rect
        which_buttons = x < title_bar->x ? &button_layout.left_buttons[0] : &button_layout.right_buttons[0];

        for (int i = 0; which_buttons[i] != META_BUTTON_FUNCTION_LAST; i++)
        {
            int rx,ry,rw,rh;
            if (!meta_get_button_position (which_buttons[i], &frame_geometry, &rx,&ry,&rw,&rh))
                continue;

            if (x >= rx && x <= rx + rw)
            {
                if (mode == MODE_HOVER && which_buttons[i] == last_active_button)
                    return TRUE;
                last_hover_state = 1;
                button_states[meta_function_to_type(last_active_button)] = META_BUTTON_STATE_NORMAL;
                button_states[meta_function_to_type(which_buttons[i])] = state;
                last_active_button = which_buttons[i];
                if (pressed)
                {
                    printf("button %d click\n",which_buttons[i]);
                    last_pressed_button = which_buttons[i];
                }
                else
                {
                    printf("button %d hover\n",which_buttons[i]);
                }
                return TRUE;
            }
        }
        return FALSE;
    }
    
    void update_title (const char *new_title)
    {
        printf("update_title %s\n", new_title);
        if (title)
            g_free (title);
        title = g_strdup (new_title);
        if (layout)
        {
            pango_layout_set_text (layout, title, -1);
        }
    }
};

// map windows pointer to decoration data
std::map<GtkWidget*,decoration_data_t*> views_data;

static void load_config ()
{
    const char *config_dir = g_build_filename (g_get_user_config_dir (), "wf-metacity-decorator", NULL);
    g_mkdir_with_parents (config_dir, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
    const char *config_file = g_build_filename (config_dir, "config.json", NULL);
    GFile *cnf = g_file_new_for_path(config_file);
    if (g_file_query_exists (cnf, NULL))
    {
        std::ifstream f(config_file);
        config = json::parse(f);
    }
    else
    {
        config = json::parse(R"(
                              {
                                "theme": "ClearlooksRe",
                                "button-layout": "menu:minimize,maximize,close",
                                "dialog-button-layout": ":close",
                                "font": "Bitstream Vera Sans Book 11"
                              }
                 )");
    }
    std::cout << config.dump(4) << std::endl;
}    

// determine the borders size, calculated on the theme and title font size, and send to plugin
static void send_borders (const char *font)
{
    int text_height;
    font_desc = pango_font_description_from_string(font);
    GtkWidget *window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    GtkStyleContext *style_gtk = gtk_widget_get_style_context (window);
    
    PangoLayout* layout = gtk_widget_create_pango_layout(window, "Prova");
    pango_layout_set_font_description(layout, font_desc);  
	pango_layout_set_wrap (layout, PANGO_WRAP_CHAR);
    pango_layout_set_auto_dir (layout, FALSE);
    pango_layout_get_pixel_size (layout, NULL, &text_height);

    meta_theme_draw_frame_test(metatheme, style_gtk, 300, 300, text_height, &fgeom, &button_layout);
    // send borders    
    update_borders(fgeom.borders.total.top, fgeom.borders.total.bottom, fgeom.borders.total.left, fgeom.borders.total.right, BORDERS_DELTA);
}
GMenuModel *make_popup()
{
    GMenu *menu = g_menu_new();
    g_menu_append_item (menu, g_menu_item_new("Minimize", NULL));
    g_menu_append_item (menu, g_menu_item_new("Maximize", NULL));
    g_menu_append_item (menu, g_menu_item_new("Close", NULL));
    return (GMenuModel*)menu;
} 

void menu_activate (GtkMenuItem *mi, gpointer user_data)
{
    const gchar *action = gtk_menu_item_get_label (mi);
    GtkWidget *window = (GtkWidget*)user_data;
    window_action (window, action);
    printf("activate %s\n", action);
} 

void popup_menu(GtkWidget *window, GdkEventButton *event)
{
    GtkWidget *window_popup = gtk_menu_new();
    GtkWidget *item = gtk_menu_item_new_with_label("minimize");
    g_signal_connect (item, "activate", (GCallback)menu_activate, (gpointer)window);
    gtk_widget_show(item);
    gtk_menu_shell_append(GTK_MENU_SHELL(window_popup), item);
    item = gtk_menu_item_new_with_label("maximize");
    g_signal_connect (item, "activate", (GCallback)menu_activate, (gpointer)window);
    gtk_widget_show(item);
    gtk_menu_shell_append(GTK_MENU_SHELL(window_popup), item);
    item = gtk_menu_item_new_with_label("close");
    g_signal_connect (item, "activate", (GCallback)menu_activate, (gpointer)window);
    gtk_widget_show(item);
    gtk_menu_shell_append(GTK_MENU_SHELL(window_popup), item);
    gtk_widget_show_all(window_popup);
    gtk_menu_popup_at_pointer (GTK_MENU(window_popup), (GdkEvent*)event);
}

static void activate (GtkApplication* app, gpointer)
{
    GdkDisplay* display = gdk_display_get_default();
    setup_protocol(display);
    GtkSettings *settings = gtk_settings_get_default ();
    // use the same cursors as wayfire
    g_object_set (settings, "gtk-cursor-theme-name", "default", NULL);
    load_config();

    std::string val = config["theme"];
    meta_theme_set_current(val.c_str(), TRUE);
    metatheme = meta_theme_get_current();
    
    /*FIXME: string handling*/
    val = config["button-layout"];
    meta_update_button_layout (val.c_str(), &button_layout);
    val = config["dialog-button-layout"];
    meta_update_button_layout (val.c_str(), &dialog_button_layout);
    val = config["font"];
    send_borders (val.c_str());
    
//    gtk_application_set_menubar (app, make_popup());
    g_application_hold(G_APPLICATION(app));
}

gboolean draw_window(GtkWindow *window, cairo_t *cr, gpointer)
{
    int client_width, client_height;
    if(!views_data.count(GTK_WIDGET(window)))
    {
        cairo_paint (cr);
        return FALSE;
    }
    cairo_set_source_rgba (cr, 0, 0, 0, 0);
    cairo_paint (cr);        
    decoration_data_t *deco = views_data[GTK_WIDGET(window)];
    
    // get the actual total window size
    gtk_window_get_size (window, &client_width, &client_height);
    
    // calculate decorated window dimensions    
    if (deco->state & STATE_MAXIMIZED)
    {
        client_width -= (fgeom.borders.visible.left + fgeom.borders.visible.right);
        client_height -= (fgeom.borders.visible.top + fgeom.borders.visible.bottom);
    }
    else
    {
        client_width -= (fgeom.borders.total.left + fgeom.borders.total.right);
        client_height -= (fgeom.borders.total.top + fgeom.borders.total.bottom);
    }
    printf("width %d - height %d\n", client_width, client_height);
    GtkStyleContext *style_gtk = gtk_widget_get_style_context (GTK_WIDGET(window));
    
    // draw decoration
    meta_theme_draw_frame (metatheme, 
                           deco->state, 
                           style_gtk, 
                           cr, 
                           client_width, 
                           client_height, 
                           deco->layout, 
                           deco->text_height, 
                           &deco->frame_geometry,
                           deco->type ? &dialog_button_layout : &button_layout,
                           deco->button_states);
                           
    return TRUE;
}

gboolean motion_notify_event (GtkWidget *window, GdkEventMotion *ev, gpointer data)
{
    if(views_data.count(window))
    {
        decoration_data_t *deco = views_data[window];
        const char *cursor_name = NULL;
        deco->current_edge = -1;
        int x = (int)ev->x;
        int y = (int)ev->y;
        MetaButtonFunction what;
        int width,height;
        
        gtk_window_get_size (GTK_WINDOW(window), &width, &height);
        if (y < deco->title_bar->y)
        {
            if (x < deco->frame_geometry.borders.total.left)
            {
                deco->current_edge = GDK_WINDOW_EDGE_NORTH_WEST;
                cursor_name = "nw-resize";
            }                
            else if (x > width - deco->frame_geometry.borders.total.right)
            {
                deco->current_edge = GDK_WINDOW_EDGE_NORTH_EAST;
                cursor_name = "ne-resize";
            }                
            else
            {
                deco->current_edge = GDK_WINDOW_EDGE_NORTH;
                cursor_name = "n-resize";
            }                
        }
        else if (deco->check_button (MODE_HOVER, x, y, META_BUTTON_STATE_PRELIGHT, 0, &what))
        {
            gtk_widget_queue_draw(window);
        }
        else 
        {
            deco->reset_button_states();
            if (y > height - deco->frame_geometry.borders.total.bottom)
            {
                if ( x < deco->frame_geometry.borders.total.left)
                {
                    deco->current_edge = GDK_WINDOW_EDGE_SOUTH_WEST;
                    cursor_name = "sw-resize";
                }                
                else if (x > width - deco->frame_geometry.borders.total.right)
                {
                    deco->current_edge = GDK_WINDOW_EDGE_SOUTH_EAST;
                    cursor_name = "se-resize";
                }
                else
                {                
                    deco->current_edge = GDK_WINDOW_EDGE_SOUTH;
                    cursor_name = "s-resize";
                }
            }                
            else if (x < deco->frame_geometry.borders.total.left)
            {
                deco->current_edge = GDK_WINDOW_EDGE_WEST;
                cursor_name = "w-resize";
            }                
            else if (x > width - deco->frame_geometry.borders.total.right)
            {
                deco->current_edge = GDK_WINDOW_EDGE_EAST;
                cursor_name = "e-resize";
            }                
        }
        GdkWindow *gdkw = gtk_widget_get_window (window);
        if (deco->current_edge >= 0)
        {
            gdk_window_set_cursor (gdkw, gdk_cursor_new_from_name (gdk_display_get_default(), cursor_name));
            deco->reset_button_states();
        }  
        else
        {
            gdk_window_set_cursor (gdkw, NULL);
        }
        gtk_widget_queue_draw(window);
    }    
    return TRUE;
}

gboolean button_press_event (GtkWidget *window, GdkEventButton *ev, gpointer data)
{
    if(ev->button != 1)
        return TRUE;
    if(views_data.count(window))
    {
        int x = (int)ev->x;
        int y = (int)ev->y;
        MetaButtonFunction what;

        decoration_data_t *deco = views_data[window];
        if( deco->current_edge >= 0)
        {
            gtk_window_begin_resize_drag (GTK_WINDOW(window), (GdkWindowEdge)deco->current_edge, ev->button, ev->x_root, ev->y_root, ev->time);
            deco->reset_button_states ();
        }            
        else if (deco->check_button (MODE_CLICK, x, y, META_BUTTON_STATE_PRESSED, 1, &what))
        {
            gtk_widget_queue_draw(window);
        }
        else
        {                   
            gtk_window_begin_move_drag (GTK_WINDOW(window), ev->button, ev->x_root, ev->y_root, ev->time);
            deco->reset_button_states ();
        }            
    }
    return TRUE;
}

gboolean button_release_event (GtkWidget *window, GdkEventButton *ev, gpointer data)
{
    if(ev->button != 1)
        return TRUE;
    if(views_data.count(window))
    {
        int x = (int)ev->x;
        int y = (int)ev->y;
        MetaButtonFunction what;

        decoration_data_t *deco = views_data[window];
        if (deco->last_pressed_button != META_BUTTON_FUNCTION_LAST)
        {
            if (deco->check_button (MODE_RELEASE, x, y, META_BUTTON_STATE_PRESSED, 0, &what))
            {
                const char *action = meta_button_function_to_string (what);
                printf("action %s\n", action);
                deco->reset_button_states();
                if (strcmp(action, "menu") == 0)
                {
                    popup_menu (window, ev);
                }
                // send button action
                window_action (window, action);
            }
            gtk_widget_queue_draw(window);
        }
    }        
    return TRUE;
}

// type: 0 toplevel, 1 dialog 
// the title has the format: __wf_decorator:<id> 
GtkWidget *create_deco_window (std::string title, uint type)
{
    GtkWidget *window;
    window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), title.c_str());
    GdkScreen *screen = gtk_window_get_screen (GTK_WINDOW(window));
    GdkVisual *visual = gdk_screen_get_rgba_visual (screen);
    gtk_widget_set_visual (window, visual);
    gtk_widget_set_events (window, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK | GDK_BUTTON1_MOTION_MASK);
    g_signal_connect (window,"draw", (GCallback)draw_window, NULL);
    g_signal_connect (window,"button-press-event", (GCallback)button_press_event, NULL);
    g_signal_connect (window,"button-release-event", (GCallback)button_release_event, NULL);
    g_signal_connect (window,"motion-notify-event", (GCallback)motion_notify_event, NULL);
    gtk_widget_show_all(window);
    printf("CREATED new decoration: %s\n", title.c_str());
    decoration_data_t *deco = new decoration_data_t (window, type);
    views_data[window] = deco;
    printf("%d windows\n", g_list_length(gtk_application_get_windows(app)));
    return window;
}

void set_title(GtkWidget *window, const char *title)
{
    g_return_if_fail(GTK_IS_WINDOW(window));
    printf("set_title - %s\n", title);
    if(views_data.count(window))
    {
        decoration_data_t *deco = views_data[window];
        deco->update_title (title);
        gtk_widget_queue_draw(window);
    }        
}

void set_view_state(GtkWidget *window, uint state)
{
    g_return_if_fail(GTK_IS_WINDOW(window));
    if(state & STATE_FOCUSED)
    {
        // reset all decorations to inactive
        for (const auto& pair : views_data) 
        {
            pair.second->state &= ~STATE_FOCUSED;           
        }
        // redraw last active, if any 
        if(GTK_IS_WINDOW(view_focused) && window != view_focused)
        {
            gtk_widget_queue_draw(view_focused);
        }
        view_focused = window;
    }        
    if(views_data.count(window))
    {
        decoration_data_t *deco = views_data[window];
        deco->state = state;
    }
    gtk_widget_queue_draw(window);
}

// free data
void set_view_unmapped(GtkWidget *window)
{
    g_return_if_fail(GTK_IS_WINDOW(window));
    if(views_data.count(window))
    {
        decoration_data_t *deco = views_data[window];
        views_data.erase (window);
        delete deco;
        gtk_widget_destroy(GTK_WIDGET(window));
        printf("%d windows\n", g_list_length(gtk_application_get_windows(app)));
    }        
}

int main(int argc, char **argv)
{
    int status;
    app = gtk_application_new("org.wf.metacity-decorator", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK (activate), NULL);
    status = g_application_run(G_APPLICATION (app), argc, argv);
    g_object_unref (app);

    return status;
}
