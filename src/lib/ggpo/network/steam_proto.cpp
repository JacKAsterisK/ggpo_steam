/* -----------------------------------------------------------------------
 * Author: JacKAsterisK
 * Modified from: GGPO - steam_proto.cpp
 */

#include "steam_proto.h"
#include "types.h"
#include "bitvector.h"

static const int STEAM_HEADER_SIZE = 28; // TODO: Find out what the actual size is, metrics will be wrong until then
static const int NUM_SYNC_PACKETS = 5;
static const int SYNC_RETRY_INTERVAL = 2000;
static const int SYNC_FIRST_RETRY_INTERVAL = 500;
static const int RUNNING_RETRY_INTERVAL = 200;
static const int KEEP_ALIVE_INTERVAL     = 200;
static const int QUALITY_REPORT_INTERVAL = 1000;
static const int NETWORK_STATS_INTERVAL  = 1000;
static const int STEAM_SHUTDOWN_TIMER = 5000;
static const int MAX_SEQ_DISTANCE = (1 << 15);

SteamProtocol::SteamProtocol() :
    _local_frame_advantage(0),
    _remote_frame_advantage(0),
    _queue(-1),
    _magic_number(0),
    _remote_magic_number(0),
    _packets_sent(0),
    _bytes_sent(0),
    _stats_start_time(0),
    _last_send_time(0),
    _shutdown_timeout(0),
    _disconnect_timeout(0),
    _disconnect_notify_start(0),
    _disconnect_notify_sent(false),
    _disconnect_event_sent(false),
    _connected(false),
    _next_send_seq(0),
    _next_recv_seq(0)
{
    _last_sent_input.init(-1, NULL, 1);
    _last_received_input.init(-1, NULL, 1);
    _last_acked_input.init(-1, NULL, 1);

    memset(&_state, 0, sizeof _state);
    memset(_peer_connect_status, 0, sizeof(_peer_connect_status));
    for (int i = 0; i < ARRAY_SIZE(_peer_connect_status); i++) {
        _peer_connect_status[i].last_frame = -1;
    }
    //memset(&_peer_addr, 0, sizeof _peer_addr);
    _oo_packet.msg = NULL;

    _send_latency = Platform::GetConfigInt("ggpo.network.delay");
    _oop_percent = Platform::GetConfigInt("ggpo.oop.percent");
}

SteamProtocol::~SteamProtocol()
{
    ClearSendQueue();
}

void SteamProtocol::Init(
    GGPOSteam *steam,
    const CSteamID& remoteSteamID,
    Poll &poll,
    int queue,
    SteamMsg::connect_status *status
) {
    _steam = steam;
    _peer_steam_id = remoteSteamID;
    _local_connect_status = status;
    SteamNetworking()->AcceptP2PSessionWithUser(_peer_steam_id);

    do {
        _magic_number = (ggpo::uint16)rand();
    } while (_magic_number == 0);

    poll.RegisterLoop(this);
}

void
SteamProtocol::SendInput(GameInput &input)
{
    if (_peer_steam_id.IsValid()) {
        if (_current_state == Running) {
            /*
             * Check to see if this is a good time to adjust for the rift...
             */
            _timesync.advance_frame(input, _local_frame_advantage, _remote_frame_advantage);

            /*
             * Save this input packet
             *
             * XXX: This queue may fill up for spectators who do not ack input packets in a timely
             * manner.  When this happens, we can either resize the queue (ug) or disconnect them
             * (better, but still ug).  For the meantime, make this queue really big to decrease
             * the odds of this happening...
             */
            _pending_output.push(input);
        }
        SendPendingOutput();
    }  
}

void
SteamProtocol::SendPendingOutput()
{
    SteamMsg *msg = new SteamMsg(SteamMsg::Input);
    int i, j, offset = 0;
    ggpo::uint8 *bits;
    GameInput last;

    if (_pending_output.size()) {
        last = _last_acked_input;
        bits = msg->u.input.bits;

        msg->u.input.start_frame = _pending_output.front().frame;
        msg->u.input.input_size = (ggpo::uint8)_pending_output.front().size;

        ASSERT(last.frame == -1 || last.frame + 1 == msg->u.input.start_frame);
        for (j = 0; j < _pending_output.size(); j++) {
            GameInput &current = _pending_output.item(j);
            if (memcmp(current.bits, last.bits, current.size) != 0) {
                ASSERT((GAMEINPUT_MAX_BYTES * GAMEINPUT_MAX_PLAYERS * 8) < (1 << BITVECTOR_NIBBLE_SIZE));
                for (i = 0; i < current.size * 8; i++) {
                    ASSERT(i < (1 << BITVECTOR_NIBBLE_SIZE));
                    if (current.value(i) != last.value(i)) {
                        BitVector_SetBit(msg->u.input.bits, &offset);
                        (current.value(i) ? BitVector_SetBit : BitVector_ClearBit)(bits, &offset);
                        BitVector_WriteNibblet(bits, i, &offset);
                    }
                }
            }
            BitVector_ClearBit(msg->u.input.bits, &offset);
            last = _last_sent_input = current;
        }
    } else {
        msg->u.input.start_frame = 0;
        msg->u.input.input_size = 0;
    }
    msg->u.input.ack_frame = _last_received_input.frame;
    msg->u.input.num_bits = (ggpo::uint16)offset;

    msg->u.input.disconnect_requested = _current_state == Disconnected;
    if (_local_connect_status) {
        memcpy(msg->u.input.peer_connect_status, _local_connect_status, sizeof(SteamMsg::connect_status) * STEAM_MSG_MAX_PLAYERS);
    } else {
        memset(msg->u.input.peer_connect_status, 0, sizeof(SteamMsg::connect_status) * STEAM_MSG_MAX_PLAYERS);
    }

    ASSERT(offset < MAX_COMPRESSED_BITS);

    SendMsg(msg);
}

void
SteamProtocol::SendInputAck()
{
    SteamMsg *msg = new SteamMsg(SteamMsg::InputAck);
    msg->u.input_ack.ack_frame = _last_received_input.frame;
    SendMsg(msg);
}

bool
SteamProtocol::GetEvent(SteamProtocol::Event &e)
{
    if (_event_queue.size() == 0) {
        return false;
    }
    e = _event_queue.front();
    _event_queue.pop();
    return true;
}


bool
SteamProtocol::OnLoopPoll(void *cookie)
{
    if (!_peer_steam_id.IsValid()) {
        return true;
    }

    unsigned int now = Platform::GetCurrentTimeMS();
    unsigned int next_interval;

    PumpSendQueue();
    switch (_current_state) {
    case Syncing:
        next_interval = (_state.sync.roundtrips_remaining == NUM_SYNC_PACKETS) ? SYNC_FIRST_RETRY_INTERVAL : SYNC_RETRY_INTERVAL;
        if (_last_send_time && _last_send_time + next_interval < now) {
            Log("No luck syncing after %d ms... Re-queueing sync packet.\n", next_interval);
            SendSyncRequest();
        }
        break;

    case Running:
        // xxx: rig all this up with a timer wrapper
        if (!_state.running.last_input_packet_recv_time || _state.running.last_input_packet_recv_time + RUNNING_RETRY_INTERVAL < now) {
            Log("Haven't exchanged packets in a while (last received:%d  last sent:%d).  Resending.\n", _last_received_input.frame, _last_sent_input.frame);
            SendPendingOutput();
            _state.running.last_input_packet_recv_time = now;
        }

        if (!_state.running.last_quality_report_time || _state.running.last_quality_report_time + QUALITY_REPORT_INTERVAL < now) {
            SteamMsg *msg = new SteamMsg(SteamMsg::QualityReport);
            msg->u.quality_report.ping = Platform::GetCurrentTimeMS();
            msg->u.quality_report.frame_advantage = (ggpo::uint8)_local_frame_advantage;
            SendMsg(msg);
            _state.running.last_quality_report_time = now;
        }

        if (!_state.running.last_network_stats_interval || _state.running.last_network_stats_interval + NETWORK_STATS_INTERVAL < now) {
            UpdateNetworkStats();
            _state.running.last_network_stats_interval =  now;
        }

        if (_last_send_time && _last_send_time + KEEP_ALIVE_INTERVAL < now) {
            Log("Sending keep alive packet\n");
            SendMsg(new SteamMsg(SteamMsg::KeepAlive));
        }

        if (_disconnect_timeout && _disconnect_notify_start && 
            !_disconnect_notify_sent && (_last_recv_time + _disconnect_notify_start < now)) {
            Log("Endpoint has stopped receiving packets for %d ms.  Sending notification.\n", _disconnect_notify_start);
            Event e(Event::NetworkInterrupted);
            e.u.network_interrupted.disconnect_timeout = _disconnect_timeout - _disconnect_notify_start;
            QueueEvent(e);
            _disconnect_notify_sent = true;
        }

        if (_disconnect_timeout && (_last_recv_time + _disconnect_timeout < now)) {
            if (!_disconnect_event_sent) {
                Log("Endpoint has stopped receiving packets for %d ms.  Disconnecting.\n", _disconnect_timeout);
                QueueEvent(Event(Event::Disconnected));
                _disconnect_event_sent = true;
            }
        }
        break;

    case Disconnected:
        if (_shutdown_timeout < now) {
            Log("Shutting down steam connection.\n");
            _peer_steam_id.Clear();
            _shutdown_timeout = 0;
        }
    }


    return true;
}

void
SteamProtocol::Disconnect()
{
    _current_state = Disconnected;
    _shutdown_timeout = Platform::GetCurrentTimeMS() + STEAM_SHUTDOWN_TIMER;
}

void
SteamProtocol::SendSyncRequest()
{
    _state.sync.random = rand() & 0xFFFF;
    SteamMsg *msg = new SteamMsg(SteamMsg::SyncRequest);
    msg->u.sync_request.random_request = _state.sync.random;
    SendMsg(msg);
}

void
SteamProtocol::SendMsg(SteamMsg *msg)
{
    LogMsg("send", msg);

    _packets_sent++;
    _last_send_time = Platform::GetCurrentTimeMS();
    _bytes_sent += msg->PacketSize();

    msg->hdr.magic = _magic_number;
    msg->hdr.sequence_number = _next_send_seq++;

    _send_queue.push(QueueEntry(Platform::GetCurrentTimeMS(), _peer_steam_id, msg));
    PumpSendQueue();
}

bool
SteamProtocol::HandlesMsg(CSteamID &to, SteamMsg *msg)
{
   return _peer_steam_id.IsValid() && to == _peer_steam_id && to != _steam->GetLocalSteamID();
}

void
SteamProtocol::OnMsg(SteamMsg *msg, int len)
{
    bool handled = false;
    typedef bool (SteamProtocol::*DispatchFn)(SteamMsg *msg, int len);
    static const DispatchFn table[] = {
        &SteamProtocol::OnInvalid,                 /* Invalid */
        &SteamProtocol::OnSyncRequest,            /* SyncRequest */
        &SteamProtocol::OnSyncReply,              /* SyncReply */
        &SteamProtocol::OnInput,                    /* Input */
        &SteamProtocol::OnQualityReport,         /* QualityReport */
        &SteamProtocol::OnQualityReply,          /* QualityReply */
        &SteamProtocol::OnKeepAlive,              /* KeepAlive */
        &SteamProtocol::OnInputAck,                /* InputAck */
    };

    // filter out messages that don't match what we expect
    ggpo::uint16 seq = msg->hdr.sequence_number;
    if (msg->hdr.type != SteamMsg::SyncRequest &&
         msg->hdr.type != SteamMsg::SyncReply) {
        if (msg->hdr.magic != _remote_magic_number) {
            LogMsg("recv rejecting", msg);
            return;
        }

        // filter out out-of-order packets
        ggpo::uint16 skipped = (ggpo::uint16)((int)seq - (int)_next_recv_seq);
        // Log("checking sequence number -> next - seq : %d - %d = %d\n", seq, _next_recv_seq, skipped);
        if (skipped > MAX_SEQ_DISTANCE) {
            Log("dropping out of order packet (seq: %d, last seq:%d)\n", seq, _next_recv_seq);
            return;
        }
    }

    _next_recv_seq = seq;
    LogMsg("recv", msg);
    if (msg->hdr.type >= ARRAY_SIZE(table)) {
        OnInvalid(msg, len);
    } else {
        handled = (this->*(table[msg->hdr.type]))(msg, len);
    }
    if (handled) {
        _last_recv_time = Platform::GetCurrentTimeMS();
        if (_disconnect_notify_sent && _current_state == Running) {
            QueueEvent(Event(Event::NetworkResumed));    
            _disconnect_notify_sent = false;
        }
    }
}

void
SteamProtocol::UpdateNetworkStats(void)
{
   int now = Platform::GetCurrentTimeMS();

   if (_stats_start_time == 0) {
      _stats_start_time = now;
   }

   int total_bytes_sent = _bytes_sent + (STEAM_HEADER_SIZE * _packets_sent);
   float seconds = (float)((now - _stats_start_time) / 1000.0);
   float Bps = total_bytes_sent / seconds;
   float steam_overhead = (float)(100.0 * (STEAM_HEADER_SIZE * _packets_sent) / _bytes_sent);

   _kbps_sent = int(Bps / 1024);

   Log("Network Stats -- Bandwidth: %.2f KBps   Packets Sent: %5d (%.2f pps)   "
       "KB Sent: %.2f    Steam Overhead: %.2f %%.\n",
       _kbps_sent, 
       _packets_sent,
       (float)_packets_sent * 1000 / (now - _stats_start_time),
       total_bytes_sent / 1024.0,
       steam_overhead);
}

void
SteamProtocol::QueueEvent(const SteamProtocol::Event &evt)
{
    LogEvent("Queuing event", evt);
    _event_queue.push(evt);
}

void
SteamProtocol::Synchronize()
{
    if (_peer_steam_id.IsValid()) {
        _current_state = Syncing;
        _state.sync.roundtrips_remaining = NUM_SYNC_PACKETS;
        SendSyncRequest();
    }
}

bool
SteamProtocol::GetPeerConnectStatus(int id, int *frame)
{
    *frame = _peer_connect_status[id].last_frame;
    return !_peer_connect_status[id].disconnected;
}

void
SteamProtocol::Log(const char *fmt, ...)
{
    char buf[1024];
    size_t offset;
    va_list args;

    sprintf_s(buf, ARRAY_SIZE(buf), "steam_proto%d | ", _queue);
    offset = strlen(buf);
    va_start(args, fmt);
    vsnprintf(buf + offset, ARRAY_SIZE(buf) - offset - 1, fmt, args);
    buf[ARRAY_SIZE(buf)-1] = '\0';
    ::Log(buf);
    va_end(args);
}

void
SteamProtocol::LogMsg(const char *prefix, SteamMsg *msg)
{
    switch (msg->hdr.type) {
    case SteamMsg::SyncRequest:
        Log("%s sync-request (%d).\n", prefix,
             msg->u.sync_request.random_request);
        break;
    case SteamMsg::SyncReply:
        Log("%s sync-reply (%d).\n", prefix,
             msg->u.sync_reply.random_reply);
        break;
    case SteamMsg::QualityReport:
        Log("%s quality report.\n", prefix);
        break;
    case SteamMsg::QualityReply:
        Log("%s quality reply.\n", prefix);
        break;
    case SteamMsg::KeepAlive:
        Log("%s keep alive.\n", prefix);
        break;
    case SteamMsg::Input:
        Log("%s game-compressed-input %d (+ %d bits).\n", prefix, msg->u.input.start_frame, msg->u.input.num_bits);
        break;
    case SteamMsg::InputAck:
        Log("%s input ack.\n", prefix);
        break;
    default:
        ASSERT(FALSE && "Unknown SteamMsg type.");
    }
}

void
SteamProtocol::LogEvent(const char *prefix, const SteamProtocol::Event &evt)
{
    switch (evt.type) {
    case SteamProtocol::Event::Synchronzied:
        Log("%s (event: Synchronzied).\n", prefix);
        break;
    }
}

bool
SteamProtocol::OnInvalid(SteamMsg *msg, int len)
{
    ASSERT(FALSE && "Invalid msg in SteamProtocol");
    return false;
}

bool
SteamProtocol::OnSyncRequest(SteamMsg *msg, int len)
{
    if (_remote_magic_number != 0 && msg->hdr.magic != _remote_magic_number) {
        Log("Ignoring sync request from unknown endpoint (%d != %d).\n", 
              msg->hdr.magic, _remote_magic_number);
        return false;
    }
    SteamMsg *reply = new SteamMsg(SteamMsg::SyncReply);
    reply->u.sync_reply.random_reply = msg->u.sync_request.random_request;
    SendMsg(reply);
    return true;
}

bool
SteamProtocol::OnSyncReply(SteamMsg *msg, int len)
{
    if (_current_state != Syncing) {
        Log("Ignoring SyncReply while not synching.\n");
        return msg->hdr.magic == _remote_magic_number;
    }

    if (msg->u.sync_reply.random_reply != _state.sync.random) {
        Log("sync reply %d != %d.  Keep looking...\n",
             msg->u.sync_reply.random_reply, _state.sync.random);
        return false;
    }

    if (!_connected) {
        QueueEvent(Event(Event::Connected));
        _connected = true;
    }

    Log("Checking sync state (%d round trips remaining).\n", _state.sync.roundtrips_remaining);
    if (--_state.sync.roundtrips_remaining == 0) {
        Log("Synchronized!\n");
        QueueEvent(SteamProtocol::Event(SteamProtocol::Event::Synchronzied));
        _current_state = Running;
        _last_received_input.frame = -1;
        _remote_magic_number = msg->hdr.magic;
    } else {
        SteamProtocol::Event evt(SteamProtocol::Event::Synchronizing);
        evt.u.synchronizing.total = NUM_SYNC_PACKETS;
        evt.u.synchronizing.count = NUM_SYNC_PACKETS - _state.sync.roundtrips_remaining;
        QueueEvent(evt);
        SendSyncRequest();
    }
    return true;
}

bool
SteamProtocol::OnInput(SteamMsg *msg, int len)
{
    /*
     * If a disconnect is requested, go ahead and disconnect now.
     */
    bool disconnect_requested = msg->u.input.disconnect_requested;
    if (disconnect_requested) {
        if (_current_state != Disconnected && !_disconnect_event_sent) {
            Log("Disconnecting endpoint on remote request.\n");
            QueueEvent(Event(Event::Disconnected));
            _disconnect_event_sent = true;
        }
    } else {
        /*
         * Update the peer connection status if this peer is still considered to be part
         * of the network.
         */
        SteamMsg::connect_status* remote_status = msg->u.input.peer_connect_status;
        for (int i = 0; i < ARRAY_SIZE(_peer_connect_status); i++) {
            ASSERT(remote_status[i].last_frame >= _peer_connect_status[i].last_frame);
            _peer_connect_status[i].disconnected = _peer_connect_status[i].disconnected || remote_status[i].disconnected;
            _peer_connect_status[i].last_frame = MAX(_peer_connect_status[i].last_frame, remote_status[i].last_frame);
        }
    }

    /*
     * Decompress the input.
     */
    int last_received_frame_number = _last_received_input.frame;
    if (msg->u.input.num_bits) {
        int offset = 0;
        ggpo::uint8 *bits = (ggpo::uint8 *)msg->u.input.bits;
        int numBits = msg->u.input.num_bits;
        int currentFrame = msg->u.input.start_frame;

        _last_received_input.size = msg->u.input.input_size;
        if (_last_received_input.frame < 0) {
            _last_received_input.frame = msg->u.input.start_frame - 1;
        }
        while (offset < numBits) {
            /*
             * Keep walking through the frames (parsing bits) until we reach
             * the inputs for the frame right after the one we're on.
             */
            ASSERT(currentFrame <= (_last_received_input.frame + 1));
            bool useInputs = currentFrame == _last_received_input.frame + 1;

            while (BitVector_ReadBit(bits, &offset)) {
                int on = BitVector_ReadBit(bits, &offset);
                int button = BitVector_ReadNibblet(bits, &offset);
                if (useInputs) {
                    if (on) {
                        _last_received_input.set(button);
                    } else {
                        _last_received_input.clear(button);
                    }
                }
            }
            ASSERT(offset <= numBits);

            /*
             * Now if we want to use these inputs, go ahead and send them to
             * the emulator.
             */
            if (useInputs) {
                /*
                 * Move forward 1 frame in the stream.
                 */
                char desc[1024];
                ASSERT(currentFrame == _last_received_input.frame + 1);
                _last_received_input.frame = currentFrame;

                /*
                 * Send the event to the emualtor
                 */
                SteamProtocol::Event evt(SteamProtocol::Event::Input);
                evt.u.input.input = _last_received_input;

                _last_received_input.desc(desc, ARRAY_SIZE(desc));

                _state.running.last_input_packet_recv_time = Platform::GetCurrentTimeMS();

                Log("Sending frame %d to emu queue %d (%s).\n", _last_received_input.frame, _queue, desc);
                QueueEvent(evt);

            } else {
                Log("Skipping past frame:(%d) current is %d.\n", currentFrame, _last_received_input.frame);
            }

            /*
             * Move forward 1 frame in the input stream.
             */
            currentFrame++;
        }
    }
    ASSERT(_last_received_input.frame >= last_received_frame_number);

    /*
     * Get rid of our buffered input
     */
    while (_pending_output.size() && _pending_output.front().frame < msg->u.input.ack_frame) {
        Log("Throwing away pending output frame %d\n", _pending_output.front().frame);
        _last_acked_input = _pending_output.front();
        _pending_output.pop();
    }
    return true;
}


bool
SteamProtocol::OnInputAck(SteamMsg *msg, int len)
{
    /*
     * Get rid of our buffered input
     */
    while (_pending_output.size() && _pending_output.front().frame < msg->u.input_ack.ack_frame) {
        Log("Throwing away pending output frame %d\n", _pending_output.front().frame);
        _last_acked_input = _pending_output.front();
        _pending_output.pop();
    }
    return true;
}

bool
SteamProtocol::OnQualityReport(SteamMsg *msg, int len)
{
    // send a reply so the other side can compute the round trip transmit time.
    SteamMsg *reply = new SteamMsg(SteamMsg::QualityReply);
    reply->u.quality_reply.pong = msg->u.quality_report.ping;
    SendMsg(reply);

    _remote_frame_advantage = msg->u.quality_report.frame_advantage;
    return true;
}

bool
SteamProtocol::OnQualityReply(SteamMsg *msg, int len)
{
    _round_trip_time = Platform::GetCurrentTimeMS() - msg->u.quality_reply.pong;
    return true;
}

bool
SteamProtocol::OnKeepAlive(SteamMsg *msg, int len)
{
    return true;
}

void
SteamProtocol::GetNetworkStats(struct GGPONetworkStats *s)
{
    s->network.ping = _round_trip_time;
    s->network.send_queue_len = _pending_output.size();
    s->network.kbps_sent = _kbps_sent;
    s->timesync.remote_frames_behind = _remote_frame_advantage;
    s->timesync.local_frames_behind = _local_frame_advantage;
}

void
SteamProtocol::SetLocalFrameNumber(int localFrame)
{
    /*
     * Estimate which frame the other guy is one by looking at the
     * last frame they gave us plus some delta for the one-way packet
     * trip time.
     */
    int remoteFrame = _last_received_input.frame + (_round_trip_time * 60 / 1000);

    /*
     * Our frame advantage is how many frames *behind* the other guy
     * we are.  Counter-intuative, I know.  It's an advantage because
     * it means they'll have to predict more often and our moves will
     * pop more frequenetly.
     */
    _local_frame_advantage = remoteFrame - localFrame;
}

int
SteamProtocol::RecommendFrameDelay()
{
    // XXX: require idle input should be a configuration parameter
    return _timesync.recommend_frame_wait_duration(false);
}


void
SteamProtocol::SetDisconnectTimeout(int timeout)
{
    _disconnect_timeout = timeout;
}

void
SteamProtocol::SetDisconnectNotifyStart(int timeout)
{
    _disconnect_notify_start = timeout;
}

void
SteamProtocol::PumpSendQueue()
{
    while (!_send_queue.empty()) {
        QueueEntry &entry = _send_queue.front();

        if (_send_latency) {
            // should really come up with a gaussian distributation based on the configured
            // value, but this will do for now.
            int jitter = (_send_latency * 2 / 3) + ((rand() % _send_latency) / 3);
            if (Platform::GetCurrentTimeMS() < _send_queue.front().queue_time + jitter) {
                break;
            }
        }
        if (_oop_percent && !_oo_packet.msg && ((rand() % 100) < _oop_percent)) {
            int delay = rand() % (_send_latency * 10 + 1000);
            Log("creating rogue oop (seq: %d  delay: %d)\n", entry.msg->hdr.sequence_number, delay);
            _oo_packet.send_time = Platform::GetCurrentTimeMS() + delay;
            _oo_packet.msg = entry.msg;
            //_oo_packet.dest_addr = entry.dest_addr; // TODO: Implement this
        } else {
            ASSERT(entry.steam_id.IsValid());

            _steam->SendTo((char *)entry.msg, entry.msg->PacketSize(), k_EP2PSendReliable, entry.steam_id);

            delete entry.msg;
        }
        _send_queue.pop();
    }
    if (_oo_packet.msg && _oo_packet.send_time < Platform::GetCurrentTimeMS()) {
        Log("sending rogue oop!");

        _steam->SendTo((char *)_oo_packet.msg, _oo_packet.msg->PacketSize(), k_EP2PSendReliable, _oo_packet.steam_id);

        delete _oo_packet.msg;
        _oo_packet.msg = NULL;
    }
}

void
SteamProtocol::ClearSendQueue()
{
    while (!_send_queue.empty()) {
        delete _send_queue.front().msg;
        _send_queue.pop();
    }
}
