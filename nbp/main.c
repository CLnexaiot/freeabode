#include "config.h"

#include <assert.h>
#include <stdio.h>

#include <zmq.h>

#include <freeabode/fabdcfg.h>
#include <freeabode/freeabode.pb-c.h>
#include <freeabode/logging.h>
#include <freeabode/security.h>
#include <freeabode/util.h>
#include "nest.h"

static const int periodic_req_interval = 30;

static const char *my_devid;
static void *my_zmq_context, *my_zmq_publisher;
static struct timespec ts_next_periodic_req;

static
void request_periodic(struct nbp_device *nbp, const struct timespec *now)
{
	timespec_add_ms(now, periodic_req_interval * 1000, &ts_next_periodic_req);
	nbp_send(nbp, NBPM_REQ_PERIODIC, NULL, 0);
#ifdef DEBUG_NBP
	applog(LOG_DEBUG, "Periodic data request");
#endif
}

#ifdef DEBUG_NBP
void debug_msg(struct nbp_device * const nbp, const struct timespec * const now, const enum nbp_message_type mtype, const void * const data, const size_t datasz)
{
	char hexdata[(datasz * 2) + 1];
	bin2hex(hexdata, data, datasz);
	applog(LOG_DEBUG, "msg %04x data %s", mtype, hexdata);
}
#endif

static
void reset_complete(struct nbp_device *nbp, const struct timespec *now, uint16_t fet_bitmask)
{
	nbp->cb_msg_fet_presence = NULL;
	applog(LOG_INFO, "Backplate reset complete");
	
	request_periodic(nbp, now);
	
	assert(fabdcfg_zmq_bind(my_devid, "events", my_zmq_publisher));
}

void msg_log(struct nbp_device *nbp, const struct timespec *now, const char *msg)
{
	applog(LOG_INFO, "Backplate: %s", msg);
}

void msg_weather(struct nbp_device *nbp, const struct timespec *now, uint16_t temperature, uint16_t humidity)
{
	int32_t fahrenheit = ((int32_t)temperature) * 90 / 5 + 32000;
	applog(LOG_INFO, "Temperature %3d.%02d C (%4d.%03d F)    Humidity: %d.%d%%", temperature / 100, temperature % 100, fahrenheit / 1000, fahrenheit % 1000, humidity / 10, humidity % 10);
	
	PbEvent pbe = PB_EVENT__INIT;
	PbWeather pb = PB_WEATHER__INIT;
	pb.has_temperature = true;
	pb.temperature = temperature;
	pb.has_humidity = true;
	pb.humidity = humidity;
	pbe.weather = &pb;
	zmq_send_protobuf(my_zmq_publisher, pb_event, &pbe, 0);
}

static
void msg_power_status(struct nbp_device * const nbp, const struct timespec * const now, const uint8_t state, const uint8_t flags, const uint8_t px0, const uint16_t u1, const uint8_t u2, const uint16_t u3, const uint16_t vi_cV, const uint16_t vo_mV, const uint16_t vb_mV, const uint8_t pins, const uint8_t wires)
{
	// output approx the same format as Nest sw so the same regex can be used to chart both
	applog(LOG_INFO, "power status: flags %02x, vi %d.%02dV, vo %d.%03dV; vb %d.%03dV", flags, vi_cV / 100, vi_cV % 100, vo_mV / 1000, vo_mV % 1000, vb_mV / 1000, vb_mV % 1000);
	
	PbEvent pbevent = PB_EVENT__INIT;
	PbBattery pbbattery = PB_BATTERY__INIT;
	pbbattery.has_charging = true;
	pbbattery.charging = !(flags & 0x40);
	pbbattery.has_voltage = true;
	pbbattery.voltage = vb_mV;
	pbevent.battery = &pbbattery;
	zmq_send_protobuf(my_zmq_publisher, pb_event, &pbevent, 0);
}

void my_nbp_control_fet_cb(struct nbp_device * const nbp, const enum nbp_fet fet, const bool connect)
{
	applog(LOG_INFO, "Setting FET %u to %d", (unsigned)fet, connect);
	
	PbEvent pbevent = PB_EVENT__INIT;
	PbSetHVACWireRequest pbwire = PB_SET_HVACWIRE_REQUEST__INIT;
	pbwire.wire = fet;
	pbwire.connect = connect;
	pbevent.wire_change = malloc(sizeof(*pbevent.wire_change));
	pbevent.n_wire_change = 1;
	*pbevent.wire_change = &pbwire;
	zmq_send_protobuf(my_zmq_publisher, pb_event, &pbevent, 0);
	free(pbevent.wire_change);
}

void handle_req(void * const s, struct nbp_device * const nbp)
{
	PbRequest *req;
	zmq_recv_protobuf(s, pb_request, req, NULL);
	PbRequestReply reply = PB_REQUEST_REPLY__INIT;
	reply.n_sethvacwiresuccess = req->n_sethvacwire;
	reply.sethvacwiresuccess = malloc(sizeof(*reply.sethvacwiresuccess) * reply.n_sethvacwiresuccess);
	for (size_t i = 0; i < req->n_sethvacwire; ++i)
		reply.sethvacwiresuccess[i] = nbp_control_fet(nbp, req->sethvacwire[i]->wire, req->sethvacwire[i]->connect);
	pb_request__free_unpacked(req, NULL);
	zmq_send_protobuf(s, pb_request_reply, &reply, 0);
	free(reply.sethvacwiresuccess);
}

void got_new_subscriber(void * const s, struct nbp_device * const nbp)
{
	zmq_msg_t msg;
	assert(!zmq_msg_init(&msg));
	assert(zmq_msg_recv(&msg, s, 0) >= 0);
	if (zmq_msg_size(&msg) < 1)
		goto out;
	
	uint8_t * const data = zmq_msg_data(&msg);
	if (!data[0])
		goto out;
	
	PbEvent pbevent = PB_EVENT__INIT;
	
	PbWeather pbweather = PB_WEATHER__INIT;
	if (nbp->has_weather)
	{
		pbweather.has_temperature = true;
		pbweather.temperature = nbp->temperature;
		pbweather.has_humidity = true;
		pbweather.humidity = nbp->humidity;
		pbevent.weather = &pbweather;
	}
	
	PbBattery pbbattery = PB_BATTERY__INIT;
	if (nbp->has_powerinfo)
	{
		pbbattery.has_charging = true;
		pbbattery.charging = !(nbp->power_flags & 0x40);
		pbbattery.has_voltage = true;
		pbbattery.voltage = nbp->vb_mV;
		pbevent.battery = &pbbattery;
	}
	
	PbSetHVACWireRequest *pbwire = malloc(sizeof(*pbwire) * PB_HVACWIRES___COUNT);
	PbSetHVACWireRequest *pbwire_top = pbwire;
	pbevent.wire_change = malloc(sizeof(*pbevent.wire_change) * PB_HVACWIRES___COUNT);
	pbevent.n_wire_change = 0;
	for (int i = 0; i < PB_HVACWIRES___COUNT; ++i)
	{
		const enum fabd_tristate asserted = nbp_get_fet_asserted(nbp, i);
		if (asserted == FTS_UNKNOWN)
			continue;
		
		pb_set_hvacwire_request__init(pbwire);
		pbwire->wire = i;
		pbwire->connect = asserted;
		
		pbevent.wire_change[pbevent.n_wire_change++] = pbwire;
		++pbwire;
	}
	
	zmq_send_protobuf(my_zmq_publisher, pb_event, &pbevent, 0);
	
	free(pbevent.wire_change);
	free(pbwire_top);
	
out:
	zmq_msg_close(&msg);
}

int main(int argc, char **argv)
{
	my_devid = fabd_common_argv(argc, argv, "nbp");
	load_freeabode_key();
	
	const char * const nbp_ttypath = fabdcfg_device_getstr(my_devid, "backplate_device") ?: "/dev/ttyO2";
	struct nbp_device *nbp = nbp_open(nbp_ttypath);
	assert(nbp);
	assert(nbp_send(nbp, NBPM_RESET, NULL, 0));
#ifdef DEBUG_NBP
	nbp->cb_msg = debug_msg;
#endif
	nbp->cb_msg_fet_presence = reset_complete;
	nbp->cb_msg_log = msg_log;
	nbp->cb_msg_power_status = msg_power_status;
	nbp->cb_msg_weather = msg_weather;
	nbp->cb_asserting_fet_control = my_nbp_control_fet_cb;
	
	my_zmq_context = zmq_ctx_new();
	start_zap_handler(my_zmq_context);
	
	void *my_zmq_ctl = zmq_socket(my_zmq_context, ZMQ_REP);
	freeabode_zmq_security(my_zmq_ctl, true);
	assert(fabdcfg_zmq_bind(my_devid, "control", my_zmq_ctl));
	
	my_zmq_publisher = zmq_socket(my_zmq_context, ZMQ_XPUB);
	zmq_setsockopt(my_zmq_publisher, ZMQ_XPUB_VERBOSE, &int_one, sizeof(int_one));
	freeabode_zmq_security(my_zmq_publisher, true);
	// NOTE: Not binding until we confirm reset
	
	timespec_clear(&ts_next_periodic_req);
	
	struct timespec ts_now, ts_timeout;
	zmq_pollitem_t pollitems[] = {
		{ .fd = nbp->_fd, .events = ZMQ_POLLIN },
		{ .socket = my_zmq_ctl, .events = ZMQ_POLLIN },
		{ .socket = my_zmq_publisher, .events = ZMQ_POLLIN },
	};
	while (true)
	{
		timespec_clear(&ts_timeout);
		clock_gettime(CLOCK_MONOTONIC, &ts_now);
		if (timespec_passed(&ts_next_periodic_req, &ts_now, &ts_timeout))
			request_periodic(nbp, &ts_now);
		if (zmq_poll(pollitems, sizeof(pollitems) / sizeof(*pollitems), timespec_to_timeout_ms(&ts_now, &ts_timeout)) <= 0)
			continue;
		if (pollitems[0].revents & ZMQ_POLLIN)
			nbp_read(nbp);
		if (pollitems[1].revents & ZMQ_POLLIN)
			handle_req(my_zmq_ctl, nbp);
		if (pollitems[2].revents & ZMQ_POLLIN)
			got_new_subscriber(my_zmq_publisher, nbp);
	}
}
