/* Small GTK launcher helper, kept separate from the emulated machine. */
#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    GtkWidget *dialog;
    int response;

    if (!gtk_init_check(&argc, &argv)) {
        fprintf(stderr, "Cannot open graphical display\n");
        return 1;
    }
    if (argc == 3 && strcmp(argv[1], "--error") == 0) {
        dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
                                        GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
                                        "%s", argv[2]);
        gtk_window_set_title(GTK_WINDOW(dialog), "F133 Radio Emulator");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return 0;
    }
    dialog = gtk_file_chooser_dialog_new("Select the F133 SPI NOR firmware",
                                        NULL, GTK_FILE_CHOOSER_ACTION_OPEN,
                                        "Cancel", GTK_RESPONSE_CANCEL,
                                        "Open", GTK_RESPONSE_ACCEPT, NULL);
    response = gtk_dialog_run(GTK_DIALOG(dialog));
    if (response == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        puts(filename);
        g_free(filename);
    }
    gtk_widget_destroy(dialog);
    return response == GTK_RESPONSE_ACCEPT ? 0 : 1;
}
