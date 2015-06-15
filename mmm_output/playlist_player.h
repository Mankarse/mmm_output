//
//  playlist_player.h
//  mmm_output
//
//  Created by Evan Wallace on 27/01/2015.
//  Copyright (c) 2015 Geko Creations. All rights reserved.
//

#ifndef MMM_OUTPUT_PLAYLIST_PLAYER_H
#define MMM_OUTPUT_PLAYLIST_PLAYER_H

#if 0
interface OutputStrawManager {
	//Old design. Audio device management should be somewhere else

	//And also something to repoll and notify of device changes?
	list<AudioDeviceProperties> getDevices();

	void set_up_option(option, value);

	AudioDevice open_device();
	//something to sync multiple output formats?
	
};

interface AudioDevice {
	void set_callback(void (*new_callback)(data_format format, size_t buf_size, unsigned char *buf, void *ud));
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

interface command_interface {
	void set_callback(void (*commandcallback)(command const *command));
};


//TODO:
// Figure out how to handle file/stream-reading errors.
// Figure out how to report on such errors.
// Possible causes:
//  File doesn't exist in first place
//  Temporary network issue
//  Permanent network issue
//  File deleted
//  File format invalid/corrupted
//  File reading/decoding slower than playback

//Possible solutions:
// Ride-through the problem, playing silence, and resume after skipping some amount of time
//      (good for streams, where the stream can be reset;
//       not so good for files, because the current location in the file will eventually be decoded, skipping will just cause more problem
//       also good for locally corrupted region of file (which can be skipped, since it cannot be understood).)
// Ride-through the problem, playing silence, and resume at the point where the silence began
//      (good for files, not so good for streams, since the source may have moved on).
// Increase buffer size, add gap between tracks (only works for relatively short tracks, and only for decoding/reading speed problems)
// Play silence for the length of the track then go to the next track
// Play go straight to the next track, or perhaps after a short fixed timeout.
// Report on the error to the 'get_status' callback.

//4 classes of problems:
//  Unrecognised format (could alternatively be data corruption, but with fewer options for recovery)
//  Data corruption
//  Data read/decode speed problems
//  Data missing.

//Data read/decode speed and data missing can sometimes be indistinguishable.

//For entirely missing/garbled data:
//  Play silence for the declared length of the track, then go to next track.
//  Periodically poll to ensure data is still missing/garbled. If data stops being missing/garbled, skip to current time, and start playing.
//  If track has no length (i.e. a stream with no bound), still do the same, I guess (unless the length is "to_end").
//  Report on situation to get_status.

//For (localised) data corruption:
// always try to smoothly transition to silence, play silence for the
// duration of the corruption, and then go back to sound.
// Perhaps transition to another mode if the corruption cannot be cleared.
// While in silence, report on such to the get_status command.

//For (localised) slow-loading:
// Smoothly transition to silence.
// Demand highest priority buffering.
// Don't underrun (?) (because it is better to have a controlled transition to silence, rather than rely on the sound driver?)
// Tell the input_stream to attempt to reload at whatever point it can (skip for streams, wait at current point for files)
// Play silence for the duration left in file since last data was retrieved.
// For files, timer resets whenever data is retrieved, for streams, timer keeps counting regardless of whether or not data was obtained.

//Corruption and slow-loading can be distinguished by the fact that the input (or output, for slow decoding) to the decoder is available for corrupted files/streams,
//but is not available for slow-loading streams.

enum failure_reason {
	SUCCESS,
	CORRUPTION,
	UNDERRUN, //Buffer not full, for whatever reason (slow reading or slow decoding).
	MISSING //Separate category for file closing/network-drive disconnect etc
};

interface input_stream {
	//add buffer-size controls?

	data_format get_format();
	
	size_t get_position();
	//Returns 0 for streaming sources with no defined play-length.
	//Length in number of frames.
	size_t get_length();
	//number of frames available from get_frames without blocking.
	size_t available_frames();
	//non-blocking request to put buffering of num_samples at the higest set_priority
	void request_prioity_buffer(size_t num_samples);
	//Set overall priority of reading from this stream.
	//Starts at -1 (will never buffer)
	//Non-negative values will buffer.
	//Suggested values
	//-1: Don't start buffering.
	//0: Not needed soon; buffer if able.(Won't begin buffering of stream sources)
	//1: Start of next track (Won't begin buffering of stream sources)
	//2: Start of next track, next track soon (Will begin buffering of stream sources)
	//3: Current track. (Will begin buffering of stream sources)
	void set_buffering_priority(int priority);
	//buf_len is in number of char, not number of frames.
	//This needs to be changed to allow for corruption to begin and end before the end of buf.
	void get_frames(char *buf, size_t buf_len, size_t *bytes_obtained, failure_reason *reason_for_failure);
};

interface input_source {
	input_stream get_input_stream(input_descriptor name);
	void discard
};

struct data_format {
	size_t channels;
	size_t rate;//Number of frames per second (a frame is a multi-channel sample)
	size_t num_bits;

	enum datatype {
		FLOAT, //Only supports 32/64 bit.
		UNSIGNED,
		SIGNED
	};
	enum weighting {
		LINEAR//,
		/*mu law etc*/
	}
	//data is assumed to always be interleaved
	//data is assumed to be PCM encoded
};
interface output_straw {
	//new design. Explicit hardware selection and resampling is handled elsewhere.
	//The playlist_player just plays files in whatever format they are provided to it in.
	//Other parts of the code handle decoding and conversion to hardware-compatible formats.
	void set_callback(void (*new_callback)(data_format format, size_t buf_size, unsigned char *buf, void *ud));

	void set_format(data_format format, size_t after_delay/*delay in frames, relative to end of last call to the callback*/);
};



struct playlist_player {
	//The callbacks must all be externally serialised.
	//playlist_player assumes that its internal state
	//is free from race-conditions.
	playlist_player(
		OutputStrawManager /*something that can produce callbacks when audio data is needed,
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

struct output_straw {
	
};

struct output_straw_manager {
	
};

struct playlist_player2 {
	
};

playlist_player2 *playlist_player2_new() {
	
};

#endif /* defined(MMM_OUTPUT_PLAYLIST_PLAYER_H) */
