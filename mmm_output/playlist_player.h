//
//  playlist_player.h
//  mmm_output
//
//  Created by Evan Wallace on 27/01/2015.
//  Copyright (c) 2015 Geko Creations. All rights reserved.
//

#ifndef __mmm_output__playlist_player__
#define __mmm_output__playlist_player__
#if 0
interface OutputStrawManager {
    //And also something to repoll and notify of device changes?
    list<AudioDeviceProperties> getDevices();
    
    void set_up_option(option, value);
    
    AudioDevice open_device();
};
interface AudioDevice {
    void install_callback();
    void uninstall_callback();
    //flush does many things (calls pending callbacks, pushes pending data
    //out to the hardware, etc.),
    //maybe it should be separated into a few functions.
    void flush();
    void start();
    void stop();
};
struct SDLUVOutputStraw {
    //Creates a libuv async event every time the SDL callback
    //occurs.
    //This async event then calls into the playlist_player to get data.
};


struct playlist_player {
    //The callbacks must all be externally serialised.
    //playlist_player assumes that its internal state
    //is free from race-conditions.
    playlist_player(
        OutputStraw /*something that can produce callbacks when audio data is needed,
                     abstracted so multiple output sinks can be created as needed*/,
        ControlInterface /*something that can produce callbacks when the commands occur
                           (Pause, Resume, UpdatePlaylist, GetState, etc)*/,
        InputSource /*Something that can produce callback when new audio data is available
                      abstracted so multiple inputs can be created as needed*/
        )
    //TODO: somehow manage 'mixer' capability to send data to multiple devices simultaneously.
    Mixer mixer;
};
#endif

#endif /* defined(__mmm_output__playlist_player__) */
