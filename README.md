# inotify-info
> Easily track down the number of inotify watches, instances, and which files are being watched.

The Linux inotify system has a few issues [1][problem1][2][problem2] and it can be difficult to debug when you for instance run out of watches. Using this app should hopefully aid you in tracking down how many inotify watches, instances, and what files are being watched.

## Screenshot
![Alt text](images/inotify-info.png?raw=true "inotify-info")

## Build
```
$ make
Building _release/inotify-info...
---- inotify-info.cpp ----
---- lfqueue/lfqueue.c ----
Linking _release/inotify-info...
```
```
$ CFG=debug make
Building _debug/inotify-info...
---- inotify-info.cpp ----
---- lfqueue/lfqueue.c ----
Linking _debug/inotify-info...
```

## Install
You are free to copy the resulting executable to any suitable location in your `$PATH`.
```
cp _release/inotify-info /usr/local/bin/
```

## Run (Prints Summary)
```
$ _release/inotify-info 
------------------------------------------------------------------------------
INotify Limits:
  max_queued_events:  16384
  max_user_instances: 128
  max_user_watches:   65536
------------------------------------------------------------------------------
     Pid  App                        Watches   Instances
     2632 systemd                         23   3
     2653 pulseaudio                       2   2
     2656 dbus-daemon                      2   1
     2987 dbus-daemon                      1   1
     3056 xfsettingsd                     56   1
     3068 xfdesktop                       10   1
     3072 wrapper-2.0                      6   1
     3091 xfce4-clipman                    1   1
     3099 xiccd                            1   1
     3343 xfce4-terminal                   1   1
     3997 xfce4-appfinder                 11   1
     4048 xdg-desktop-portal               1   1
     4086 xdg-desktop-portal-gtk          56   1
   205668 vivaldi-bin                      8   1
   205705 vivaldi-bin                      2   1
------------------------------------------------------------------------------
Total inotify Watches:   181
Total inotify Instances: 18
------------------------------------------------------------------------------
```

## Run with Appname Filter
```
$ _release/inotify-info xfce4
------------------------------------------------------------------------------
INotify Limits:
  max_queued_events:  16384
  max_user_instances: 128
  max_user_watches:   65536
------------------------------------------------------------------------------
     Pid  App                        Watches   Instances
     2632 systemd                         23   3
     2653 pulseaudio                       2   2
     2656 dbus-daemon                      2   1
     2987 dbus-daemon                      1   1
     3056 xfsettingsd                     56   1
     3068 xfdesktop                       10   1
     3072 wrapper-2.0                      6   1
     3091 xfce4-clipman                    1   1
      94111050 [10304h]
     3099 xiccd                            1   1
     3343 xfce4-terminal                   1   1
      71048655 [10304h]
     3997 xfce4-appfinder                 11   1
      94111468 [10304h]  15339430 [10304h]  14554799 [10304h]  70254617 [10304h]  70254684 [10304h]  16786993 [10304h]  14551253 [10304h]  14550430 [10304h]  70254647 [10304h]  70254646 [10304h]
      92275589 [10304h]
     4048 xdg-desktop-portal               1   1
     4086 xdg-desktop-portal-gtk          56   1
   205668 vivaldi-bin                      8   1
   205705 vivaldi-bin                      2   1
------------------------------------------------------------------------------
Total inotify Watches:   181
Total inotify Instances: 18
------------------------------------------------------------------------------

Searching '/' for listed inodes... (8 threads)
 14550430 [10304h] /usr/share/applications/
 14551253 [10304h] /usr/local/share/
 14554799 [10304h] /usr/share/xfce4/
 15339430 [10304h] /usr/share/desktop-directories/
 16786993 [10304h] /usr/share/xfce4/applications/
 70254617 [10304h] /home/mikesart/.local/share/
 70254646 [10304h] /home/mikesart/.config/menus/
 70254647 [10304h] /home/mikesart/.config/menus/applications-merged/
 70254684 [10304h] /home/mikesart/.local/share/applications/
 71048655 [10304h] /home/mikesart/.config/xfce4/terminal/
 92275589 [10304h] /etc/xdg/menus/
 94111050 [10304h] /home/mikesart/.config/xfce4/panel/
 94111468 [10304h] /home/mikesart/.cache/xfce4/xfce4-appfinder/
```
## Run with Specific Pid(s)
```
$ _release/inotify-info 3997
------------------------------------------------------------------------------
INotify Limits:
  max_queued_events:  16384
  max_user_instances: 128
  max_user_watches:   65536
------------------------------------------------------------------------------
     Pid  App                        Watches   Instances
     2632 systemd                         23   3
     2653 pulseaudio                       2   2
     2656 dbus-daemon                      2   1
     2987 dbus-daemon                      1   1
     3056 xfsettingsd                     56   1
     3068 xfdesktop                       10   1
     3072 wrapper-2.0                      6   1
     3091 xfce4-clipman                    1   1
     3099 xiccd                            1   1
     3343 xfce4-terminal                   1   1
     3997 xfce4-appfinder                 11   1
      94111468 [10304h]  15339430 [10304h]  14554799 [10304h]  70254617 [10304h]  70254684 [10304h]  16786993 [10304h]  14551253 [10304h]  14550430 [10304h]  70254647 [10304h]  70254646 [10304h]
      92275589 [10304h]
     4048 xdg-desktop-portal               1   1
     4086 xdg-desktop-portal-gtk          56   1
   205668 vivaldi-bin                      8   1
   205705 vivaldi-bin                      2   1
------------------------------------------------------------------------------
Total inotify Watches:   181
Total inotify Instances: 18
------------------------------------------------------------------------------

Searching '/' for listed inodes... (8 threads)
 14550430 [10304h] /usr/share/applications/
 14551253 [10304h] /usr/local/share/
 14554799 [10304h] /usr/share/xfce4/
 15339430 [10304h] /usr/share/desktop-directories/
 16786993 [10304h] /usr/share/xfce4/applications/
 70254617 [10304h] /home/mikesart/.local/share/
 70254646 [10304h] /home/mikesart/.config/menus/
 70254647 [10304h] /home/mikesart/.config/menus/applications-merged/
 70254684 [10304h] /home/mikesart/.local/share/applications/
 92275589 [10304h] /etc/xdg/menus/
 94111468 [10304h] /home/mikesart/.cache/xfce4/xfce4-appfinder/
```

## Credits

[lfqueue][lfqueue] is [BSD-2-Clause License][bsd]


[problem1]: https://code.visualstudio.com/docs/setup/linux#_visual-studio-code-is-unable-to-watch-for-file-changes-in-this-large-workspace-error-enospc  
[problem2]: https://unix.stackexchange.com/questions/15509/whos-consuming-my-inotify-resources  
[lfqueue]:  https://github.com/Taymindis/lfqueue
[bsd]:      https://github.com/Taymindis/lfqueue/blob/master/LICENSE
