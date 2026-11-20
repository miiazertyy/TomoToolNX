#pragma once
// ui_tour.h — TomoToolNX (development only)
// A scripted walk through the app, for the README's clips. With debug_ui=1 in
// config.ini and a tour.txt next to the .nro, the app plays the script's
// button presses and taps; between "record" and "stop" its clock moves exactly
// 1/30 s per frame (however long a frame takes) and every frame is saved as a
// PNG, so the clips come out smooth on any machine.
//
// tour.txt, one command per line ('#' starts a comment):
//   wait 1.5                     seconds of the app's clock
//   press A                      a button: A B X Y L R ZL ZR PLUS MINUS UP DOWN LEFT RIGHT
//   hold DOWN 0.8                a button held that long
//   tap 640 360                  a finger on the screen (1280 x 720 points)
//   drag 400 300 800 300 0.6     a finger moved over that time
//   record opening               save frames to /switch/TomoToolNX/rec/opening/
//   stop                         stop saving
//   quit                         close the app

#include <SDL2/SDL.h>

namespace Tour {

// Read the script (false if there is none). Call once, after the config.
bool Load(const char* path);
bool Active();
// Once per frame, after Motion::BeginFrame() and before Input::Poll().
void Step();
// Before a frame is drawn: while recording, it is drawn into a texture.
void BeginRender(SDL_Renderer* ren);
// Once per frame, after drawing and before presenting: saves the frame and
// puts it on the screen.
void Capture(SDL_Renderer* ren);
// The script ended with "quit".
bool QuitRequested();

}  // namespace Tour
