json = dependency('nlohmann_json')
wf_metacity_decorator = executable('wf-metacity-decorator',
    ['main.cpp', 'protocol.cpp', 'theme.c', 'gradient.c', 'theme-parser.c', 'boxes.c'],
    dependencies: [gtk3, gdk_pixbuf, wayland_client, wf_client_protos, json],
    install: true, install_dir:'/usr/bin')
