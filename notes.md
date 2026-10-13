I removed tests entirely. use examples as tests instead. remove anything left from it.

we can make the template repo directly a preset template. and have a complete documentation, guide in it.

make critical plugin loading warnings that are about manifest and preset structure, like circular dependencies, a load time errors instead of showing placeholders. not found can show placeholders, but critical errors are about invalid structure.

we can have an option to return layout to preset default.

we can add an option to close the application.

add udp or other protocols to low level net plugin.

maybe a plugin that will abstract the text rendering that for example tty and a text editor can use. they both place text to a grid.

always keep plugins abstraction in mind. that will help us porting to other platforms.

use more of C23 features if you need, like attributes, static_assert, enum types or nullptr.

maybe we can get inspiration from raygui for UI.

more UI features can be borrowed from unity ui components, or even html. a toggle, dropdown, slider, selector, containers, lists, etc. but I dont want std plugins to be bloated and too domain specific. so it will not include panels like hierarchy, inspector, etc. but it can include a generic plugins that can be used to build those.

also add a maximized identifier like you did with locked. or an icon, an emoji works better for both of these.

lets note that I want theme support, so add 2 themes by default, one dark and one light. defaulting to system.

the app stops responding sometimes, it fixes when I wait for it for a few seconds though.

image and audio having their own draw and play seems wrong. these should be just loaders like gtlf does. the responsibility to play or show the media should be a nother plugin, like draw plugin for images, and mixer for audio. just like we do in gltf. but still in model example we are placing pixels by hand. why not just add primitive rendering options to draw plugin itself?

so separate loading and executing in any context. primitives are known by the executor. loading makes file formats compatible with it.

if we already have os module, why we have fs? if it provides more features or easier to use functions than that, or planning to do so, keep it. but I dont want multiple plugins having the same responsibility. ALWAYS KEEP THE RESPONSIBILITY AND CONSISTENCY RULES IN YOUR MIND, IN BOTH MACRO AND MICRO SCALE.

why do we have onShutDown function registration while we already have destroy callback?

why do we have SettingDesc and SettingExplanation separately?

why panel and Panel are 2 separate classes?

why there are both members of a class and getter / setter functions for the same member? like panel.title and panel.getTitle()?

add caching to examples and std

make the os window handle and buttons enabled by default. because its currently not possible to move or resize main app or popped out windows.

make the window shut down instead of showing blank when there is no panels left to show.

currently closing a pop out windows or tiling them to each other gives errors and feel clunky, sometimes it is not showing iling highlight. tested on 2 machines (omarchy and opensuse tumbleweed):
AddressSanitizer:DEADLYSIGNAL
=================================================================
==203899==ERROR: AddressSanitizer: SEGV on unknown address 0x000000000000 (pc 0x000000000000 bp 0x7ffd4f520f74 sp 0x7ffd4f520e88 T0)
==203899==Hint: pc points to the zero page.
==203899==The signal is caused by a READ memory access.
==203899==Hint: address points to the zero page.
    #0 0x000000000000  (<unknown module>)

==203899==Register values:
rax = 0x00007b036fc7ef78  rbx = 0x00007cb37eff3b80  rcx = 0x0000000000000000  rdx = 0x00007ffd4f520f74  
rdi = 0x00007c437efe3240  rsi = 0x00007b437f007090  rbp = 0x00007ffd4f520f74  rsp = 0x00007ffd4f520e88  
 r8 = 0x0000000000000000   r9 = 0x00007ffd4f521044  r10 = 0xffffffffffffffff  r11 = 0x0000000000000000  
r12 = 0xffffffffffffffff  r13 = 0x0000000000000002  r14 = 0x0000000000000000  r15 = 0x00007cb37eff3cb0  
AddressSanitizer can not provide additional info.
SUMMARY: AddressSanitizer: SEGV (<unknown module>) 
==203899==ABORTING

shorten right click menu options because they enlarge the rectangle

I added notes as new commits to all 3 repos. search for !!!, ??? and todo