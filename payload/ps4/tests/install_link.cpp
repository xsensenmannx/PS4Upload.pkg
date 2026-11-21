/*
 * P1-Spike (nur Link-Test, kein Lauf): belegt, dass ps4app/install.h mit den
 * orbis-Headern uebersetzt und gegen libSceAppInstUtil/libSceUserService
 * linkt, ohne libSceBgft fest zu ziehen (das kommt per dlsym).
 *
 * Die Aufrufe stehen in main(), damit die statischen Helfer aus install.h
 * wirklich im Objekt landen und ihre BGFT-Symbole in der Link-Zeile stehen.
 * Ausgefuehrt wird das ELF nicht.
 */

#include <string.h>

#include "install.h"

int main(int argc, char **argv)
{
    PkgInfo info;
    const char *path = argc > 1 ? argv[1] : "/data/ps4upload/spike.pkg";

    memset(&info, 0, sizeof(info));
    info.size = (unsigned long long)(unsigned int)argc;

    install_reset(path);
    install_begin(path, info, argc);
    install_poll((unsigned long long)(unsigned int)argc);
    install_mark_upload(1, 2);

    return install_busy() ? 0 : 0;
}
