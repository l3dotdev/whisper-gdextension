#include "whisper_microphone_transcriber.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/audio_server.hpp>
using namespace godot;

/* --- WhisperMicrophoneTranscriber implementation --- */

WhisperMicrophoneTranscriber::WhisperMicrophoneTranscriber() {
	bus_name = "WhisperMicCapture_" + String::num_int64((int64_t)this);
	mtx.instantiate();
	sem.instantiate();
}

WhisperMicrophoneTranscriber::~WhisperMicrophoneTranscriber() {
	stop();
	_cleanup_audio_bus();
}

void WhisperMicrophoneTranscriber::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_whisper", "whisper"), &WhisperMicrophoneTranscriber::set_whisper);
	ClassDB::bind_method(D_METHOD("get_whisper"), &WhisperMicrophoneTranscriber::get_whisper);

	ClassDB::bind_method(D_METHOD("set_step_ms", "step_ms"), &WhisperMicrophoneTranscriber::set_step_ms);
	ClassDB::bind_method(D_METHOD("get_step_ms"), &WhisperMicrophoneTranscriber::get_step_ms);

	ClassDB::bind_method(D_METHOD("set_length_ms", "length_ms"), &WhisperMicrophoneTranscriber::set_length_ms);
	ClassDB::bind_method(D_METHOD("get_length_ms"), &WhisperMicrophoneTranscriber::get_length_ms);

	ClassDB::bind_method(D_METHOD("set_keep_ms", "keep_ms"), &WhisperMicrophoneTranscriber::set_keep_ms);
	ClassDB::bind_method(D_METHOD("get_keep_ms"), &WhisperMicrophoneTranscriber::get_keep_ms);

	ClassDB::bind_method(D_METHOD("reset_bus_name", "keep_ms"), &WhisperMicrophoneTranscriber::reset_bus_name);
	ClassDB::bind_method(D_METHOD("set_bus_name", "bus_name"), &WhisperMicrophoneTranscriber::set_bus_name);
	ClassDB::bind_method(D_METHOD("get_bus_name"), &WhisperMicrophoneTranscriber::get_bus_name);

	ClassDB::bind_method(D_METHOD("start"), &WhisperMicrophoneTranscriber::start);
	ClassDB::bind_method(D_METHOD("stop"), &WhisperMicrophoneTranscriber::stop);
	ClassDB::bind_method(D_METHOD("is_running"), &WhisperMicrophoneTranscriber::is_running);

	ClassDB::bind_method(D_METHOD("push_audio_chunk", "samples"), &WhisperMicrophoneTranscriber::push_audio_chunk);

	// internal thread function (must be callable for GDExtension Thread)
	ClassDB::bind_method(D_METHOD("_thread_func"), &WhisperMicrophoneTranscriber::_thread_func);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "whisper", PROPERTY_HINT_RESOURCE_TYPE, "WhisperFull"), "set_whisper", "get_whisper");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "step_ms", PROPERTY_HINT_RANGE, "500,10000,100"), "set_step_ms", "get_step_ms");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "length_ms", PROPERTY_HINT_RANGE, "1000,30000,100"), "set_length_ms", "get_length_ms");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "keep_ms", PROPERTY_HINT_RANGE, "0,2000,50"), "set_keep_ms", "get_keep_ms");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "bus_name", PROPERTY_HINT_NONE, "The name of the audio bus used for transcription"), "set_bus_name", "get_bus_name");

	ADD_SIGNAL(MethodInfo("transcription_text", PropertyInfo(Variant::STRING, "text")));
	ADD_SIGNAL(MethodInfo("transcription_segment", PropertyInfo(Variant::OBJECT, "segment", PROPERTY_HINT_RESOURCE_TYPE, "WhisperSegment")));
	ADD_SIGNAL(MethodInfo("transcription_started"));
	ADD_SIGNAL(MethodInfo("transcription_stopped"));
	ADD_SIGNAL(MethodInfo("transcription_error", PropertyInfo(Variant::STRING, "error")));
}

void WhisperMicrophoneTranscriber::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_INTERNAL_PROCESS: {
			_emit_pending_results();
		} break;
		case NOTIFICATION_EXIT_TREE: {
			stop();
		} break;
	}
}

/* --- property getters/setters --- */

void WhisperMicrophoneTranscriber::set_whisper(const Ref<WhisperFull> &p_whisper) {
	if (running.is_set()) {
		ERR_PRINT("[WhisperMicrophoneTranscriber] cannot change whisper while running");
		return;
	}
	whisper = p_whisper;
}

Ref<WhisperFull> WhisperMicrophoneTranscriber::get_whisper() const {
	return whisper;
}

void WhisperMicrophoneTranscriber::set_step_ms(int p_step_ms) {
	step_ms = CLAMP(p_step_ms, 500, 10000);
}

int WhisperMicrophoneTranscriber::get_step_ms() const {
	return step_ms;
}

void WhisperMicrophoneTranscriber::set_length_ms(int p_length_ms) {
	length_ms = CLAMP(p_length_ms, 1000, 30000);
}

int WhisperMicrophoneTranscriber::get_length_ms() const {
	return length_ms;
}

void WhisperMicrophoneTranscriber::set_keep_ms(int p_keep_ms) {
	keep_ms = CLAMP(p_keep_ms, 0, 2000);
}

int WhisperMicrophoneTranscriber::get_keep_ms() const {
	return keep_ms;
}

void WhisperMicrophoneTranscriber::reset_bus_name() {
	if (running.is_set()) {
		ERR_PRINT("[WhisperMicrophoneTranscriber] cannot change bus name while running");
		return;
	}
	bus_name = "WhisperMicCapture_" + String::num_int64((int64_t)this);
	use_custom_bus = false;
}

void WhisperMicrophoneTranscriber::set_bus_name(const String &p_bus_name) {
	if (running.is_set()) {
		ERR_PRINT("[WhisperMicrophoneTranscriber] cannot change bus name while running");
		return;
	}
	bus_name = p_bus_name;
	use_custom_bus = true;
}

String WhisperMicrophoneTranscriber::get_bus_name() const {
	return bus_name;
}

/* --- audio bus setup --- */

void WhisperMicrophoneTranscriber::_setup_audio_stream() {
	_setup_audio_bus();

	Ref<AudioStreamMicrophone> mic_stream;
	mic_stream.instantiate();

	// create AudioStreamPlayer with microphone input
	mic_player = memnew(AudioStreamPlayer);
	mic_player->set_stream(mic_stream);
	mic_player->set_bus(bus_name);
	mic_player->set_autoplay(true);
	add_child(mic_player);
}

void WhisperMicrophoneTranscriber::_setup_audio_bus() {
	AudioServer *audio_server = AudioServer::get_singleton();
	if (!audio_server) {
		return;
	}

	bus_index = audio_server->get_bus_index(bus_name);
	if (bus_index >= 0) {
		int effect_count = audio_server->get_bus_effect_count(bus_index);
		Ref<AudioEffect> last_effect = audio_server->get_bus_effect(bus_index, effect_count - 1);

		if (last_effect.get_class_static() != "AudioEffectCapture") {
			ERR_PRINT("[WhisperMicrophoneTranscriber] last bus effect on custom bus must be an AudioEffectCapture effect");
		}

		audio_effect = last_effect;
		audio_effect->set_buffer_length(float(length_ms) / 1000.0f + 1.0f);
		
		return;
	}

	// create a new audio bus for microphone capture
	bus_index = audio_server->get_bus_count();
	audio_server->add_bus(bus_index);
	audio_server->set_bus_name(bus_index, bus_name);
	audio_server->set_bus_mute(bus_index, true); // mute to prevent feedback

	// add capture effect
	audio_effect.instantiate();
	audio_effect->set_buffer_length(float(length_ms) / 1000.0f + 1.0f);
	audio_server->add_bus_effect(bus_index, audio_effect);
}

void WhisperMicrophoneTranscriber::_cleanup_audio_bus() {
	if (mic_player) {
		mic_player->stop();
		mic_player->queue_free();
		mic_player = nullptr;
	}

	if (!use_custom_bus) {
		AudioServer *audio_server = AudioServer::get_singleton();
		if (audio_server && bus_index >= 0) {
			// find and remove the bus by name (index might have changed)
			int idx = audio_server->get_bus_index(bus_name);
			if (idx >= 0) {
				audio_server->remove_bus(idx);
			}
			bus_index = -1;
		}
		
		audio_effect.unref();
	}
}

/* --- control methods --- */

bool WhisperMicrophoneTranscriber::start() {
	if (running.is_set()) {
		ERR_PRINT("[WhisperMicrophoneTranscriber] already running");
		return false;
	}

	if (whisper.is_null()) {
		ERR_PRINT("[WhisperMicrophoneTranscriber] whisper is not set");
		emit_signal("transcription_error", String("whisper is not set"));
		return false;
	}

	if (!whisper->is_initialized()) {
		if (!whisper->init()) {
			ERR_PRINT("[WhisperMicrophoneTranscriber] failed to initialize whisper");
			emit_signal("transcription_error", String("failed to initialize whisper"));
			return false;
		}
	}

	// setup audio capture
	_setup_audio_stream();

	// clear buffers
	{
		mtx->lock();

		pcmf32_buffer.clear();
		pcmf32_old.clear();
		pending_texts.clear();
		pending_segments.clear();
		mtx->unlock();
	}

	accumulated_time = 0.0f;

	// start thread
	should_stop.clear();
	running.set();

	worker_thread.instantiate();
	worker_thread->start(Callable(this, "_thread_func"));

	// enable internal process to emit results on main thread
	set_process_internal(true);

	emit_signal("transcription_started");
	return true;
}

void WhisperMicrophoneTranscriber::stop() {
	if (!running.is_set()) {
		return;
	}

	should_stop.set();

	sem->post(); // wake up thread if waiting
	if (worker_thread.is_valid() && worker_thread->is_started()) {
		worker_thread->wait_to_finish();
	}
	worker_thread.unref();

	running.clear();
	set_process_internal(false);

	_cleanup_audio_bus();

	emit_signal("transcription_stopped");
}

bool WhisperMicrophoneTranscriber::is_running() const {
	return running.is_set();
}

/* --- manual audio input --- */

void WhisperMicrophoneTranscriber::push_audio_chunk(const PackedFloat32Array &p_samples) {
	if (!running.is_set()) {
		return;
	}

	mtx->lock();
	pcmf32_buffer.append_array(p_samples);
	mtx->unlock();
}

/* --- thread function --- */

void WhisperMicrophoneTranscriber::_thread_func() {
	const int whisper_sample_rate = 16000;
	const int n_samples_step = int(float(step_ms) * whisper_sample_rate / 1000.0f);

	while (!should_stop.is_set()) {
		// capture audio from microphone
		if (audio_effect.is_valid()) {
			int frames_available = audio_effect->get_frames_available();
			if (frames_available > 0) {
				PackedVector2Array stereo_data = audio_effect->get_buffer(frames_available);
				audio_effect->clear_buffer();

				AudioServer *audio_server = AudioServer::get_singleton();
				int sample_rate = audio_server->get_mix_rate();

				PackedFloat32Array mono_data = WhisperFull::convert_stereo_to_mono_16khz(sample_rate, stereo_data);

				if (!mono_data.is_empty()) {
					mtx->lock();
					pcmf32_buffer.append_array(mono_data);
					mtx->unlock();
				}
			}
		}

		// check if we have enough samples to process
		bool should_process = false;
		{
			mtx->lock();
			should_process = pcmf32_buffer.size() >= n_samples_step;
			mtx->unlock();
		}

		if (should_process) {
			_process_audio();
		} else {
			// sleep a bit to avoid busy waiting
			OS::get_singleton()->delay_usec(10000); // 10ms
		}
	}
}

void WhisperMicrophoneTranscriber::_process_audio() {
	const int whisper_sample_rate = 16000;
	const int n_samples_step = int(float(step_ms) * whisper_sample_rate / 1000.0f);
	const int n_samples_len = int(float(length_ms) * whisper_sample_rate / 1000.0f);
	const int n_samples_keep = int(float(keep_ms) * whisper_sample_rate / 1000.0f);

	PackedFloat32Array pcmf32_new;
	PackedFloat32Array pcmf32_old_copy;

	// get audio from buffer
	{
		mtx->lock();

		if (pcmf32_buffer.size() < n_samples_step) {
			mtx->unlock();
			return;
		}

		// take audio from buffer
		pcmf32_new = pcmf32_buffer;
		pcmf32_buffer.clear();

		pcmf32_old_copy = pcmf32_old;
		mtx->unlock();
	}

	// calculate how many old samples to keep (to mitigate word boundary issues)
	int n_samples_take = MIN(pcmf32_old_copy.size(), MAX(0, n_samples_keep + n_samples_len - pcmf32_new.size()));

	// combine old and new samples
	PackedFloat32Array pcmf32;
	pcmf32.resize(n_samples_take + pcmf32_new.size());

	float *pcmf32_ptr = pcmf32.ptrw();

	// copy old samples
	if (n_samples_take > 0) {
		const float *old_ptr = pcmf32_old_copy.ptr();
		int old_size = pcmf32_old_copy.size();
		for (int i = 0; i < n_samples_take; i++) {
			pcmf32_ptr[i] = old_ptr[old_size - n_samples_take + i];
		}
	}

	// copy new samples
	const float *new_ptr = pcmf32_new.ptr();
	for (int i = 0; i < pcmf32_new.size(); i++) {
		pcmf32_ptr[n_samples_take + i] = new_ptr[i];
	}

	// save samples for next iteration
	{
		mtx->lock();
		pcmf32_old = pcmf32;
		mtx->unlock();
	}

	// transcribe
	int result = whisper->transcribe(pcmf32);

	if (result == 0) {
		// get results
		String full_text = whisper->get_full_text();
		LocalVector<Ref<WhisperSegment>> segments;
		whisper->get_all_segments_native(segments);

        //print_line(full_text);

		// queue results to be emitted on main thread
		mtx->lock();

		if (!full_text.strip_edges().is_empty()) {
			pending_texts.push_back(full_text);
		}

		for (int i = 0; i < segments.size(); i++) {
			Ref<WhisperSegment> seg = segments[i];
			if (seg.is_valid() && !seg->get_text().strip_edges().is_empty()) {
				pending_segments.push_back(seg);
			}
		}
		mtx->unlock();
	}
}

/* --- emit results on main thread --- */

void WhisperMicrophoneTranscriber::_emit_pending_results() {
	LocalVector<String> texts_to_emit;
	LocalVector<Ref<WhisperSegment>> segments_to_emit;

	{
		mtx->lock();
		texts_to_emit = pending_texts;
		segments_to_emit = pending_segments;
		pending_texts.clear();
		pending_segments.clear();
		mtx->unlock();
	}

	for (const String &text : texts_to_emit) {
		emit_signal("transcription_text", text);
	}

	for (const Ref<WhisperSegment> &seg : segments_to_emit) {
		emit_signal("transcription_segment", seg);
	}
}
