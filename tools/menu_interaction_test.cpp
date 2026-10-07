#include "ui/Menu.h"
#include "gfx/Renderer.h"
#include <SDL.h>
#include <cstdio>
using namespace ht2;
int main() {
    Renderer renderer;if(!renderer.init(1280,720,true))return 1;
    Menu menu;if(!menu.load("original"))return 2;
    menu.loadArt(renderer);menu.setViewport(1280,720);
    int failures=0,checks=0;
    auto check=[&](bool ok,const char* label){++checks;failures+=!ok;printf("%s %s\n",ok?"PASS":"FAIL",label);};
    menu.activateName("graph");check(menu.page()=="graph","graphics card opens");
    // The 800x600 virtual menu is centred in the 1280x720 viewport.
    menu.handleMouse(160+(27+62)*1.2f,(202+62)*1.2f,true);
    check(menu.page()=="control","visible outer control socket switches open card by mouse");
    menu.handleKey(SDLK_ESCAPE);check(menu.page().empty(),"Escape returns from card to ring");
    menu.activateName("graph");const int selected=menu.selected();menu.handleKey(SDLK_DOWN);
    check(menu.page().empty()&&menu.selected()!=selected,"arrow releases card focus and navigates outer menu");
    menu.activateName("exit");menu.handleMouse(160+(27+62)*1.2f,(332+62)*1.2f,true);
    check(menu.page()=="exitconf"&&menu.takeActivated().empty(),"exit popup blocks outer demo button");
    menu.handleKey(SDLK_DOWN);check(menu.page()=="exitconf","exit popup remains modal for arrows");
    menu.handleKey(SDLK_ESCAPE);check(menu.page().empty(),"Escape cancels exit without quitting");
    printf("RESULT menu interaction %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
