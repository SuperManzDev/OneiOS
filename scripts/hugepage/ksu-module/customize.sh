#!/system/bin/sh
# KernelSU sources this while installing the module.
ui_print "- OneiOS native support: on-demand only"
ui_print "- No post-fs-data, service, boot-completed, display, or framework action is installed"
ui_print "- After reboot run as root: oneios-native status"
ui_print "- Explicit preparation: oneios-native prepare 4096"
set_perm "$MODPATH/system/bin/oneios-native" 0 0 0755
set_perm "$MODPATH/action.sh" 0 0 0755
set_perm_recursive "$MODPATH/bin" 0 0 0755 0644
set_perm "$MODPATH/target.conf" 0 0 0600
