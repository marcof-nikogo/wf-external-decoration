#ifndef PROTOCOL_HPP
#define PROTOCOL_HPP

#include <gdk/gdkwayland.h>
#include <gtk/gtk.h>
#include <string>
#include <stdio.h>

void setup_protocol(GdkDisplay *display);

/* Before mapping (widget.show_all()) the window title must be set EXACTLY as the parameter title */
GtkWidget *create_deco_window(std::string title, uint32_t type);

void set_title       (GtkWidget *window, const char *title);
void set_view_state(GtkWidget *window, uint32_t state);
void set_view_unmapped(GtkWidget *window);
//void window_destroyed(GtkWidget *window);
void update_borders(uint32_t left, uint32_t right, uint32_t bottom, uint32_t top, uint32_t delta);
void window_action(GtkWidget *window, const char *action);
void update_title_rect(GtkWidget *window, uint32_t top, uint32_t bottom, uint32_t left, uint32_t right);
void reset_deco_states(GtkWidget *window);
void update_buttons(GtkWidget *window, int x, int y, int pressed);

#endif /* end of include guard: PROTOCOL_HPP */
