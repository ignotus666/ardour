/*
 * Copyright (C) 2021 Paul Davis <paul@linuxaudiosystems.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef __ardour_triggerbox_h__
#define __ardour_triggerbox_h__

#include <pthread.h>

#include <atomic>
#include <map>
#include <vector>
#include <string>
#include <exception>

#include <glibmm/threads.h>

#include "pbd/crossthread.h"
#include "pbd/pcg_rand.h"
#include "pbd/pool.h"
#include "pbd/properties.h"
#include "pbd/ringbuffer.h"
#include "pbd/stateful.h"

#include "temporal/beats.h"
#include "temporal/bbt_time.h"
#include "temporal/tempo.h"

#include "ardour/midi_model.h"
#include "ardour/midi_state_tracker.h"
#include "ardour/processor.h"
#include "ardour/segment_descriptor.h"
#include "ardour/types.h"
#include "ardour/types_convert.h"

#include "ardour/libardour_visibility.h"

class XMLNode;

namespace RubberBand {
	class RubberBandStretcher;
}

namespace ARDOUR {

class Session;
class AudioRegion;
class MidiRegion;
class TriggerBox;
class SideChain;

typedef uint32_t color_t;

class LIBARDOUR_API Trigger : public PBD::Stateful {
  public:
	enum State {
		/* This is the initial state for a Trigger, and means that it is not
		 *doing anything at all
		 */
		Stopped,
		/* A Trigger in this state has been chosen by its parent TriggerBox
		 * (e.g. because of a bang() call that put it in the queue), a Trigger in
		 * this state is waiting for the next occurence of its quantization to
		 *  occur before transitioning to Running
		 */
		WaitingToStart,
		/* a Trigger in this state is going to deliver data during calls
		 *  to its ::run() method.
		 */
		Running,
		/* a Trigger in this state was running, has been re-triggered e.g. by a
		 *  ::bang() call with LaunchStyle set to Repeat, and is waiting for the
		 *  next occurence of its quantization to occur before transitioning
		 *  back to Running.
		 */
		WaitingForRetrigger,
		/* a Trigger in this state is delivering data during calls to ::run(), but
		 *  is waiting for the next occurence of its quantization to occur when it will
		 *transition to Stopping and then Stopped.
		 */
		WaitingToStop,
		/* a Trigger in this state was Running but noticed that it should stop
		 * during the current call to ::run(). By the end of that call, it will
		 * have transitioned to Stopped.
		 */
		Stopping,
		/* a Trigger in this state has played all of its data and is
		 * now silent-filling until we reach the "true end" of the trigger
		 */
		Playout,
	};

	Trigger (uint32_t index, TriggerBox&);
	virtual ~Trigger() {}

	static void make_property_quarks ();

	void set_name (std::string const &);
	std::string name() const { return _name; }

	void set_stretchable (bool yn);
	bool stretchable () const { return _stretchable; }

	void set_scene_isolated (bool isolate);
	bool scene_isolated () const { return _isolated; }

	/* Calling ::bang() will cause this Trigger to be placed in its owning
	   TriggerBox's queue.
	*/
	void bang ();

	/* Calling ::unbang() will cause a running Trigger to begin the process
	   of stopping. If the Trigger is not running, it will move it to a
	   full Stopped state.
	*/
	void unbang ();

	/* Calling ::request_stop() to stop a Trigger at the earliest possible
	 * opportunity, rather than at the next quantization point.
	 */
	void request_stop ();

	virtual pframes_t run (BufferSet&, samplepos_t start_sample, samplepos_t end_sample,
	                       Temporal::Beats const & start, Temporal::Beats const & end,
	                       pframes_t nframes, pframes_t offset, bool first, double bpm, bool can_clear) = 0;
	virtual void set_start (timepos_t const &) = 0;
	virtual void set_end (timepos_t const &) = 0;
	virtual void set_length (timecnt_t const &) = 0;
	virtual void reload (BufferSet&, void*) = 0;
	virtual void io_change () {}

	virtual double position_as_fraction() const = 0;
	virtual void set_expected_end_sample (Temporal::TempoMap::SharedPtr const &, Temporal::BBT_Time const &, samplepos_t) = 0;

	/* because follow actions involve probability is it easier to code the will-not-follow case */

	bool will_not_follow() const;
	bool will_follow() const { return !will_not_follow(); }

	virtual bool probably_oneshot () const = 0;

	virtual timepos_t start_offset () const = 0; /* offset from start of data */
	virtual timepos_t current_length() const = 0; /* offset from start() */
	virtual timepos_t natural_length() const = 0; /* offset from start() */

	void process_state_requests (BufferSet& bufs, pframes_t dest_offset);

	bool active() const { return _state >= Running; }
	State state() const { return _state; }

	enum LaunchStyle {
		OneShot,  /* mouse down/NoteOn starts; mouse up/NoteOff ignored */
		ReTrigger, /* mouse down/NoteOn starts or retriggers; mouse up/NoteOff */
		Gate,     /* runs till mouse up/note off then to next quantization */
		Toggle,   /* runs till next mouse down/NoteOn */
		Repeat,   /* plays only quantization extent until mouse up/note off */
	};

	LaunchStyle launch_style() const;
	void set_launch_style (LaunchStyle);

	FollowAction follow_action (uint32_t n) const { assert (n < 2); return n ? _follow_action1 : _follow_action0; }
	void set_follow_action (FollowAction, uint32_t n);

	color_t  color() const { return _color; }
	void set_color (color_t);

	void set_region (boost::shared_ptr<Region>, bool use_thread = true);
	void clear_region ();
	virtual int set_region_in_worker_thread (boost::shared_ptr<Region>) = 0;
	boost::shared_ptr<Region> region() const { return _region; }

	Temporal::BBT_Offset quantization() const;
	void set_quantization (Temporal::BBT_Offset const &);

	uint32_t index() const { return _index; }

	/* Managed by TriggerBox, these record the time that the trigger is
	 * scheduled to start or stop at. Computed in
	 * Trigger::maybe_compute_next_transition().
	 */
	samplepos_t transition_samples;
	Temporal::Beats transition_beats;

	XMLNode& get_state (void);
	int set_state (const XMLNode&, int version);

	void maybe_compute_next_transition (samplepos_t start_sample, Temporal::Beats const & start, Temporal::Beats const & end, pframes_t& nframes, pframes_t& dest_offset, bool passthru);

	void set_next_trigger (int n);
	int next_trigger() const { return _next_trigger; }

	void set_use_follow_length (bool);
	bool use_follow_length() const { return _use_follow_length; }

	void set_follow_length (Temporal::BBT_Offset const &);
	Temporal::BBT_Offset follow_length() const { return _follow_length; }

	void set_follow_action_probability (int zero_to_a_hundred);
	int  follow_action_probability() const { return _follow_action_probability; }

	virtual void set_legato_offset (timepos_t const & offset) = 0;
	virtual timepos_t current_pos() const = 0;
	void set_legato (bool yn);
	bool legato () const { return _legato; }

	/* any non-zero value will work for the default argument, and means
	   "use your own launch quantization". BBT_Offset (0, 0, 0) means what
	   it says: start immediately
	*/
	void startup (Temporal::BBT_Offset const & start_quantization = Temporal::BBT_Offset (9, 3,0));
	virtual void shutdown (BufferSet& bufs, pframes_t dest_offset);
	virtual void jump_start ();
	virtual void jump_stop (BufferSet& bufs, pframes_t dest_offset);
	void begin_stop (bool explicit_stop = false);

	bool explicitly_stopped() const { return _explicitly_stopped; }

	uint32_t loop_count() const { return _loop_cnt; }
	uint32_t follow_count() const { return _follow_count; }
	void set_follow_count (uint32_t n);

	void set_ui (void*);
	void* ui () const { return _ui; }

	TriggerBox& box() const { return _box; }

	double estimated_tempo() const { return _estimated_tempo; }

	Temporal::Meter meter() const { return _meter; }

	gain_t gain() const { return _gain; }
	void set_gain (gain_t);

	void set_velocity_gain (gain_t g) {_pending_velocity_gain=g;}

	float midi_velocity_effect() const { return _midi_velocity_effect; }
	void set_midi_velocity_effect (float);

	enum StretchMode { /* currently mapped to the matching RubberBand::RubberBandStretcher::Option  */
		Crisp,
		Mixed,
		Smooth,
	};

	void set_pending (Trigger*);
	Trigger* swap_pending (Trigger*);

	static Trigger * const MagicClearPointerValue;

	virtual SegmentDescriptor get_segment_descriptor () const = 0;

	static void request_trigger_delete (Trigger* t);

  protected:
	struct UIRequests {
		std::atomic<bool> stop;
		UIRequests() : stop (false) {}
	};

	boost::shared_ptr<Region> _region;
	TriggerBox&               _box;
	UIRequests                _requests;
	State                     _state;
	std::atomic<int>          _bang;
	std::atomic<int>          _unbang;
	uint32_t                  _index;
	int                       _next_trigger;
	uint32_t                  _loop_cnt; /* how many times in a row has this played */
	void*                     _ui;
	bool                      _explicitly_stopped;

	gain_t                    _pending_velocity_gain;
	gain_t                    _velocity_gain;

	/* properties controllable by the user */

	PBD::Property<LaunchStyle>          _launch_style;
	PBD::Property<FollowAction>         _follow_action0;
	PBD::Property<FollowAction>         _follow_action1;
	PBD::Property<int>                  _follow_action_probability; /* 1 .. 100 */
	PBD::Property<uint32_t>             _follow_count;
	PBD::Property<Temporal::BBT_Offset> _quantization;
	PBD::Property<Temporal::BBT_Offset> _follow_length;
	PBD::Property<bool>                 _use_follow_length;
	PBD::Property<bool>                 _legato;
	PBD::Property<std::string>          _name;
	PBD::Property<gain_t>               _gain;
	PBD::Property<float>                _midi_velocity_effect;
	PBD::Property<bool>                 _stretchable;
	PBD::Property<bool>                 _isolated;
	PBD::Property<color_t>              _color;
	PBD::Property<StretchMode>          _stretch_mode;

	bool cue_launched;

	/* computed from data */

	double                    _estimated_tempo;  //TODO:  this should come from the MIDI file
	double                    _segment_tempo;  //TODO: this will likely get stored in the SegmentDescriptor for audio triggers

	double                    _barcnt; /* our estimate of the number of bars in the region */
	Temporal::Meter           _meter;

	samplepos_t                expected_end_sample;
	Temporal::BBT_Offset      _start_quantization;
	std::atomic<Trigger*>     _pending;

	void when_stopped_during_run (BufferSet& bufs, pframes_t dest_offset);
	void set_region_internal (boost::shared_ptr<Region>);
	virtual void retrigger() = 0;
	virtual void _startup (Temporal::BBT_Offset const &);
};

typedef boost::shared_ptr<Trigger> TriggerPtr;

class LIBARDOUR_API AudioTrigger : public Trigger {
  public:
	AudioTrigger (uint32_t index, TriggerBox&);
	~AudioTrigger ();

	pframes_t run (BufferSet&, samplepos_t start_sample, samplepos_t end_sample, Temporal::Beats const & start, Temporal::Beats const & end, pframes_t nframes, pframes_t offset, bool first, double bpm, bool);

	StretchMode stretch_mode() const { return _stretch_mode; }
	void set_stretch_mode (StretchMode);

	double segment_tempo() const { return _segment_tempo; }
	void set_segment_tempo (double t);

	Temporal::Meter segment_meter() const { return _meter; }  //TODO: might be different?
	void set_segment_meter(Temporal::Meter const &);  //TODO: disambiguated from a future midi::metrum

	double segment_barcnt () { return _barcnt; }
	void set_segment_barcnt (double count);

	void set_start (timepos_t const &);
	void set_end (timepos_t const &);
	void set_legato_offset (timepos_t const &);
	timepos_t current_pos() const;
	void set_length (timecnt_t const &);
	timepos_t start_offset () const; /* offset from start of data */
	timepos_t current_length() const; /* offset from start of data */
	timepos_t natural_length() const; /* offset from start of data */
	void reload (BufferSet&, void*);
	void io_change ();
	bool probably_oneshot () const;

	double position_as_fraction() const;

	int set_region_in_worker_thread (boost::shared_ptr<Region>);
	void jump_start ();
	void jump_stop (BufferSet& bufs, pframes_t dest_offset);

	XMLNode& get_state (void);
	int set_state (const XMLNode&, int version);

	RubberBand::RubberBandStretcher* stretcher() { return (_stretcher); }

	SegmentDescriptor get_segment_descriptor () const;
	void set_expected_end_sample (Temporal::TempoMap::SharedPtr const &, Temporal::BBT_Time const &, samplepos_t);

	bool stretching () const;

  protected:
	void retrigger ();

  private:
	struct Data : std::vector<Sample*> {
		samplecnt_t length;

		Data () : length (0) {}
	};

	Data        data;
	RubberBand::RubberBandStretcher*  _stretcher;
	samplepos_t _start_offset;

	/* computed after data is reset */

	samplepos_t last_sample;   /* where the data runs out, relative to the start of the data, compare with read_index */

	/* computed during run */

	samplecnt_t read_index;
	samplecnt_t process_index;
	samplepos_t final_sample;  /* where we stop playing, relative to the timeline */
	samplepos_t _legato_offset;
	samplecnt_t retrieved;
	samplecnt_t got_stretcher_padding;
	samplecnt_t to_pad;
	samplecnt_t to_drop;

	void drop_data ();
	int load_data (boost::shared_ptr<AudioRegion>);
	void estimate_tempo ();
	void setup_stretcher ();
	void _startup (Temporal::BBT_Offset const &);
};


class LIBARDOUR_API MIDITrigger : public Trigger {
  public:
	MIDITrigger (uint32_t index, TriggerBox&);
	~MIDITrigger ();

	pframes_t run (BufferSet&, samplepos_t start_sample, samplepos_t end_sample, Temporal::Beats const & start_beats, Temporal::Beats const & end_beats, pframes_t nframes, pframes_t offset, bool passthru, double bpm, bool can_clear);

	void set_start (timepos_t const &);
	void set_end (timepos_t const &);
	void set_legato_offset (timepos_t const &);
	timepos_t current_pos() const;
	void set_length (timecnt_t const &);
	timepos_t start_offset () const;
	timepos_t end() const;            /* offset from start of data */
	timepos_t current_length() const; /* offset from start of data */
	timepos_t natural_length() const; /* offset from start of data */
	void reload (BufferSet&, void*);
	bool probably_oneshot () const;

	double position_as_fraction() const;

	int set_region_in_worker_thread (boost::shared_ptr<Region>);
	void jump_start ();
	void shutdown (BufferSet& bufs, pframes_t dest_offset);
	void jump_stop (BufferSet& bufs, pframes_t dest_offset);

	XMLNode& get_state (void);
	int set_state (const XMLNode&, int version);

	SegmentDescriptor get_segment_descriptor () const;
	void set_expected_end_sample (Temporal::TempoMap::SharedPtr const &, Temporal::BBT_Time const &, samplepos_t);

  protected:
	void retrigger ();

  private:
	PBD::ID data_source;
	MidiStateTracker tracker;
	PBD::ScopedConnection content_connection;

	Temporal::Beats final_beat;

	Temporal::DoubleableBeats data_length;   /* using timestamps from data */
	Temporal::DoubleableBeats last_event_beats;

	Temporal::BBT_Offset _start_offset;
	Temporal::BBT_Offset _legato_offset;

	MidiModel::const_iterator iter;
	boost::shared_ptr<MidiModel> model;

	int load_data (boost::shared_ptr<MidiRegion>);
	void compute_and_set_length ();
	void _startup (Temporal::BBT_Offset const &);
};

class LIBARDOUR_API TriggerBoxThread
{
  public:
	TriggerBoxThread ();
	~TriggerBoxThread();

	static void init_request_pool() { Request::init_pool(); }

	void set_region (TriggerBox&, uint32_t slot, boost::shared_ptr<Region>);
	void request_delete_trigger (Trigger* t);

	void summon();
	void stop();
	void wait_until_finished();

  private:
	static void* _thread_work(void *arg);
	void*         thread_work();

	enum RequestType {
		Quit,
		SetRegion,
		DeleteTrigger
	};

	struct Request {

		Request (RequestType t) : type (t) {}

		RequestType type;
		/* for set region */
		TriggerBox* box;
		uint32_t slot;
		boost::shared_ptr<Region> region;
		/* for DeleteTrigger */
		Trigger* trigger;

		void* operator new (size_t);
		void  operator delete (void* ptr, size_t);

		static MultiAllocSingleReleasePool* pool;
		static void init_pool ();
	};

	pthread_t thread;
	PBD::RingBuffer<Request*>  requests;

	CrossThreadChannel _xthread;
	void queue_request (Request*);
	void delete_trigger (Trigger*);
};


class LIBARDOUR_API TriggerBox : public Processor
{
  public:
	TriggerBox (Session&, DataType dt);
	~TriggerBox ();

	void run (BufferSet& bufs, samplepos_t start_sample, samplepos_t end_sample, double speed, pframes_t nframes, bool result_required);
	bool can_support_io_configuration (const ChanCount& in, ChanCount& out);
	bool configure_io (ChanCount in, ChanCount out);

	int32_t order() const { return _order; }
	void set_order(int32_t n);

	typedef std::vector<TriggerPtr> Triggers;

	TriggerPtr trigger (Triggers::size_type);

	bool bang_trigger (TriggerPtr);
	bool unbang_trigger (TriggerPtr);
	void add_trigger (TriggerPtr);

	void set_pending (uint32_t slot, Trigger*);

	XMLNode& get_state (void);
	int set_state (const XMLNode&, int version);

	void set_from_path (uint32_t slot, std::string const & path);
	void set_from_selection (uint32_t slot, boost::shared_ptr<Region>);

	DataType data_type() const { return _data_type; }

	void stop_all_immediately ();
	void stop_all_quantized ();

	TriggerPtr currently_playing() const { return _currently_playing; }

	void clear_all_triggers ();
	void set_all_follow_action (ARDOUR::FollowAction const &, uint32_t n=0);
	void set_all_launch_style (ARDOUR::Trigger::LaunchStyle);
	void set_all_quantization (Temporal::BBT_Offset const&);
	void set_all_probability (int zero_to_a_hundred);

	/* Returns a negative value is there is no active Trigger, or a value between 0
	 * and 1.0 if there is, corresponding to the value of position_as_fraction() for
	 *  the active Trigger.
	 */
	double position_as_fraction() const;

	void queue_explict (uint32_t);
	TriggerPtr get_next_trigger ();
	TriggerPtr peek_next_trigger ();

	void add_midi_sidechain ();
	void update_sidechain_name ();

	bool pass_thru() const { return _requests.pass_thru; }
	void set_pass_thru (bool yn);

	void request_reload (int32_t slot, void*);
	void set_region (uint32_t slot, boost::shared_ptr<Region> region);

	/* valid only within the ::run() call tree */
	int32_t active_scene() const { return _active_scene; }

	PBD::Signal1<void,uint32_t> TriggerSwapped;

	enum TriggerMidiMapMode {
		AbletonPush,
		SequentialNote,
		ByMidiChannel
	};

	static Temporal::BBT_Offset assumed_trigger_duration () { return _assumed_trigger_duration; }
	static void set_assumed_trigger_duration (Temporal::BBT_Offset const &);

	static TriggerMidiMapMode midi_map_mode () { return _midi_map_mode; }
	static void set_midi_map_mode (TriggerMidiMapMode m);

	static int first_midi_note() { return _first_midi_note; }
	static void set_first_midi_note (int n);

	static void init ();

	static TriggerBoxThread* worker;

	static void start_transport_stop (Session&);

  private:
	struct Requests {
		std::atomic<bool> stop_all;
		std::atomic<bool> pass_thru;

		Requests () : stop_all (false), pass_thru (false) {}
	};

	static Temporal::BBT_Offset _assumed_trigger_duration;

	DataType _data_type;
	int32_t _order;
	Glib::Threads::RWLock trigger_lock; /* protects all_triggers */
	Triggers all_triggers;

	typedef std::vector<Trigger*> PendingTriggers;
	PendingTriggers pending;

	PBD::RingBuffer<uint32_t> explicit_queue; /* user queued triggers */
	TriggerPtr               _currently_playing;
	Requests                 _requests;
	bool                     _stop_all;
	bool                     _pass_thru;
	int32_t                  _active_scene;

	boost::shared_ptr<SideChain> _sidechain;

	PBD::PCGRand _pcg;

	/* These four are accessed (read/write) only from process() context */

	void drop_triggers ();
	void process_ui_trigger_requests ();
	void process_midi_trigger_requests (BufferSet&);
	int determine_next_trigger (uint32_t n);
	void stop_all ();

	void maybe_swap_pending (uint32_t);

	int note_to_trigger (int node, int channel);

	void note_on (int note_number, int velocity);
	void note_off (int note_number, int velocity);

	void reconnect_to_default ();
	void parameter_changed (std::string const &);

	static int _first_midi_note;
	static TriggerMidiMapMode _midi_map_mode;

	struct Request {
		enum Type {
			Use,
			Reload,
		};

		Type type;

		/* cannot use a union here because we need Request to have a
		 * "trivial" constructor.
		 */

		TriggerPtr trigger;
		void* ptr;
		int32_t slot;

		Request (Type t) : type (t) {}

		static MultiAllocSingleReleasePool* pool;
		static void init_pool();

		void* operator new (size_t);
		void  operator delete (void* ptr, size_t);
	};

	typedef PBD::RingBuffer<Request*> RequestBuffer;
	RequestBuffer requests;

	void process_requests (BufferSet&);
	void process_request (BufferSet&, Request*);

	void reload (BufferSet& bufs, int32_t slot, void* ptr);

	PBD::ScopedConnection stop_all_connection;

	static void init_pool();

	static std::atomic<int> active_trigger_boxes;
};

class TriggerReference
{
public:
	TriggerReference () : box (0), slot (0) {}
	TriggerReference (ARDOUR::TriggerBox& b, uint32_t s) : box (&b), slot (s) {}

	boost::shared_ptr<ARDOUR::Trigger> trigger() const { assert (box); return box->trigger (slot); }

	ARDOUR::TriggerBox* box;
	uint32_t            slot;
};

namespace Properties {
	LIBARDOUR_API extern PBD::PropertyDescriptor<bool> running;
	LIBARDOUR_API extern PBD::PropertyDescriptor<bool> passthru;
	LIBARDOUR_API extern PBD::PropertyDescriptor<bool> legato;
	LIBARDOUR_API extern PBD::PropertyDescriptor<bool> use_follow_length;
	LIBARDOUR_API extern PBD::PropertyDescriptor<Temporal::BBT_Offset> quantization;
	LIBARDOUR_API extern PBD::PropertyDescriptor<Temporal::BBT_Offset> follow_length;
	LIBARDOUR_API extern PBD::PropertyDescriptor<Trigger::LaunchStyle> launch_style;
	LIBARDOUR_API extern PBD::PropertyDescriptor<FollowAction> follow_action0;
	LIBARDOUR_API extern PBD::PropertyDescriptor<FollowAction> follow_action1;
	LIBARDOUR_API extern PBD::PropertyDescriptor<Trigger::StretchMode> stretch_mode;
	LIBARDOUR_API extern PBD::PropertyDescriptor<uint32_t> follow_count;
	LIBARDOUR_API extern PBD::PropertyDescriptor<int> follow_action_probability;
	LIBARDOUR_API extern PBD::PropertyDescriptor<float> velocity_effect;
	LIBARDOUR_API extern PBD::PropertyDescriptor<gain_t> gain;
	LIBARDOUR_API extern PBD::PropertyDescriptor<uint32_t> currently_playing;
	LIBARDOUR_API extern PBD::PropertyDescriptor<bool> stretchable;
	LIBARDOUR_API extern PBD::PropertyDescriptor<bool> isolated;

	LIBARDOUR_API extern PBD::PropertyDescriptor<bool> tempo_meter; /* only used to transmit changes, not storage */
}


} // namespace ARDOUR

namespace PBD {
DEFINE_ENUM_CONVERT(ARDOUR::FollowAction::Type);
DEFINE_ENUM_CONVERT(ARDOUR::Trigger::LaunchStyle);
DEFINE_ENUM_CONVERT(ARDOUR::Trigger::StretchMode);
} /* namespace PBD */


#endif /* __ardour_triggerbox_h__ */
