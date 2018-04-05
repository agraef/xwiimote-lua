
/* xwiilua.c: simplified interface to libxwiimote in Lua (AG, 2017-12-23)

   Copyright (c) 2017 by Albert Graef <aggraef@gmail.com>

   Copying and distribution of this file, with or without modification,
   are permitted in any medium without royalty provided the copyright
   notice and this notice are preserved.  This file is offered as-is,
   without any warranty. */

/* This presents a simplified version of the xwiimote interface
   (http://dvdhrm.github.io/xwiimote/) optimized for an interpreted language
   like Lua. Devices are identified using handles (1-based indices into the
   devices table); a function to return the known devices as a Lua table is
   provided. The devices, once opened, can be polled for key events, which
   also keeps track of the various kinds of motion events. The Wiimote can
   generate a *lot* of motion events, reporting every single event isn't
   really practical in Lua, so we provide functions to read the current motion
   data instead. An application can then just keep polling for key events and
   read the motion data it needs at regular intervals.

   This module should implement most of the standard features supported by
   libxwiimote, but note that at present only the core Wiimote input devices
   (accelerometer and IR tracker), the Motion-Plus gyroscope and the Nunchuk
   have actually been tested. This matches the Wii Remote Plus with an
   attached Nunchuk, which is the configuration I have and can test. XXXTODO:
   guitar and drum movement events haven't been implemented yet, but should be
   easy to add if anyone needs them. If you notice any bugs or can contribute
   code to better support the Guitar and Drum Controllers, please let me know
   or send me a pull request at Github. */

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>

#include <xwiimote.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

// We support a maximum of NDEV different devices right now. Each open device
// has a corresponding handle in the range 1..NDEV.
#define NDEV 10

typedef struct {
  struct xwii_iface *iface; // xwiimote iface descriptor
  int fds_num; // number of file descriptors (1 if open, 0 otherwise)
  struct pollfd fds[1]; // file descriptor used to poll the device
  // movement data (pro stores movement data for both the classic and pro
  // controllers)
  struct xwii_event_abs accel, motion, nunchuk_accel, nunchuk_stick,
    ir[4], pro[2], board[4];
} devhandle;

static devhandle devh[NDEV];

// Return a table with the names of the devices known to the system. (Note
// that this will report all devices connected to the system, but only the
// first NDEV of these will actually be accessible using the routines provided
// below.)
static int l_xwii_list(lua_State *L)
{
  struct xwii_monitor *mon;
  char *ent;
  int i = 0;

  mon = xwii_monitor_new(false, false);
  if (!mon) {
    fprintf(stderr, "xwii_list: cannot create monitor\n");
    return 0;
  }

  lua_newtable(L);
  while ((ent = xwii_monitor_poll(mon))) {
    lua_pushinteger(L, ++i);
    lua_pushstring(L, ent);
    lua_settable(L, -3);
    free(ent);
  }

  xwii_monitor_unref(mon);
  return 1;
}

// Return the name of a device, given by its index in the range 1..NDEV.
// Return value is a string which must be freed by the caller.
static char *get_dev(int num)
{
  struct xwii_monitor *mon;
  char *ent;
  int i = 0;

  mon = xwii_monitor_new(false, false);
  if (!mon) {
    return NULL;
  }
  while ((ent = xwii_monitor_poll(mon))) {
    if (++i == num) break;
    free(ent);
  }

  xwii_monitor_unref(mon);

  return ent;
}

// Open the device given its index in the range 1..NDEV. Returns the device
// handle (the index) if the device can be opened, 0 otherwise. In the current
// implementation, each device can only be opened once; if the device is
// already open, 0 is returned instead, indicating failure.
static int l_xwii_open(lua_State *L)
{
  int num = (int)luaL_checknumber(L, 1);
  char *path = 0;
  if (num < 1 || num > NDEV || !(path = get_dev(num))) {
    fprintf(stderr, "xwii_open: cannot find device #%d\n", num);
    lua_pushinteger(L, 0);
    return 1;
  }
  if (devh[num-1].fds_num) { // device is already open
    free(path);
    lua_pushinteger(L, 0);
    return 1;
  }
  int ret = xwii_iface_new(&devh[num-1].iface, path);
  if (ret) {
    fprintf(stderr, "xwii_open: cannot create xwii_iface '%s' err:%d\n",
	    path, ret);
    free(path);
    lua_pushinteger(L, 0);
    return 1;
  }
  ret = xwii_iface_open(devh[num-1].iface,
			xwii_iface_available(devh[num-1].iface) |
			XWII_IFACE_WRITABLE);
  if (ret) {
    fprintf(stderr, "xwii_open: cannot open interface '%s' err: %d\n",
	    path, ret);
    xwii_iface_unref(devh[num-1].iface);
    free(path);
    lua_pushinteger(L, 0);
    return 1;
  }
  ret = xwii_iface_watch(devh[num-1].iface, true);
  if (ret) {
    fprintf(stderr, "xwii_open: cannot initialize hotplug watch descriptor on interface '%s' err: %d\n",
	    path, ret);
  }
  memset(&devh[num-1].accel, 0, sizeof(devh[num-1].accel));
  memset(&devh[num-1].motion, 0, sizeof(devh[num-1].motion));
  memset(&devh[num-1].nunchuk_accel, 0, sizeof(devh[num-1].nunchuk_accel));
  memset(&devh[num-1].nunchuk_stick, 0, sizeof(devh[num-1].nunchuk_stick));
  memset(devh[num-1].fds, 0, sizeof(devh[num-1].fds));
  devh[num-1].fds[0].fd = xwii_iface_get_fd(devh[num-1].iface);
  devh[num-1].fds[0].events = POLLIN;
  devh[num-1].fds_num = 1;
  free(path);
  lua_pushinteger(L, num);
  return 1;
}

// Close the device given by its handle. This never fails and doesn't return
// anything.
static int l_xwii_close(lua_State *L)
{
  int num = (int)luaL_checknumber(L, 1);
  // check that device is open
  if (num >= 1 && num <= NDEV && devh[num-1].fds_num) {
    xwii_iface_close(devh[num-1].iface,
		     xwii_iface_opened(devh[num-1].iface));
    xwii_iface_unref(devh[num-1].iface);
    devh[num-1].fds_num = 0;
  }
  return 0;
}

// Check which interfaces the device supports. This is a bitmask, see
// xwii_iface_type in the xwiimote.h header file for possible values.
static int l_xwii_info(lua_State *L)
{
  int num = (int)luaL_checknumber(L, 1);
  if (num >= 1 && num <= NDEV && devh[num-1].fds_num) {
    lua_pushinteger(L, xwii_iface_available(devh[num-1].iface));
  } else {
    lua_pushinteger(L, 0);
  }
  return 1;
}

// Retrieve the battery capacity (unsigned 8 bit value).
static int l_xwii_get_battery(lua_State *L)
{
  int num = (int)luaL_checknumber(L, 1);
  if (num >= 1 && num <= NDEV && devh[num-1].fds_num) {
    uint8_t capacity;
    int ret = xwii_iface_get_battery(devh[num-1].iface, &capacity);
    if (ret) {
      fprintf(stderr, "xwii_get_battery: cannot read battery capacity\n");
      lua_pushnil(L);
    } else {
      lua_pushinteger(L, capacity);
    }
  } else {
    lua_pushinteger(L, 0);
  }
  return 1;
}

// Retrieve the status of the 4 LEDs as a bitmask.
static int l_xwii_get_leds(lua_State *L)
{
  int num = (int)luaL_checknumber(L, 1);
  if (num >= 1 && num <= NDEV && devh[num-1].fds_num) {
    uint8_t mask = 0;
    int i, ret = 0;
    for (i = ret = 0; i < 4 && !ret; i++) {
      bool flag;
      ret = xwii_iface_get_led(devh[num-1].iface, XWII_LED(i+1), &flag);
      if (!ret && flag) mask |= 1<<i;
    }
    if (ret) {
      fprintf(stderr, "xwii_get_leds: cannot read LED state\n");
      lua_pushnil(L);
    } else {
      lua_pushinteger(L, mask);
    }
  } else {
    lua_pushinteger(L, 0);
  }
  return 1;
}

// Set the status of the 4 LEDs from a bitmask.
static int l_xwii_set_leds(lua_State *L)
{
  int num = (int)luaL_checknumber(L, 1);
  uint8_t mask = (uint8_t)luaL_checknumber(L, 2);
  if (num >= 1 && num <= NDEV && devh[num-1].fds_num) {
    int i;
    for (i = 0; i < 4; i++) {
      bool flag = !!(mask & (1<<i));
      int ret = xwii_iface_set_led(devh[num-1].iface, XWII_LED(i+1), flag);
      if (ret) {
	fprintf(stderr, "xwii_set_leds: cannot write LED state\n");
	return 0;
      }
    }
  }
  return 0;
}

// Set the status of the rumble motor (0 = off, nonzero = on).
static int l_xwii_rumble(lua_State *L)
{
  int num = (int)luaL_checknumber(L, 1);
  int flag = (int)luaL_checknumber(L, 2);
  if (num >= 1 && num <= NDEV && devh[num-1].fds_num) {
    int ret = xwii_iface_rumble(devh[num-1].iface, !!flag);
    if (ret) {
      fprintf(stderr, "xwii_rumble: cannot set rumble motor state\n");
    }
  }
  return 0;
}

// Polls the device for key events and reports them. Returns nil if the device
// isn't open, if there's an error reading from the device or if no key event
// is currently available. Otherwise returns a single key event as a table
// consisting of the event id, key id and key status. This should be called in
// regular intervals since it also records the current motion information
// which can be queried using the corresponding functions below.
static int l_xwii_poll(lua_State *L)
{
  int num = (int)luaL_checknumber(L, 1);
  if (num >= 1 && num <= NDEV && devh[num-1].fds_num) {
    struct xwii_event event;
    int ret = poll(devh[num-1].fds, devh[num-1].fds_num, -1);
    if (ret < 0) {
      if (errno != EINTR) {
	ret = -errno;
	fprintf(stderr, "xwii_poll: cannot poll fds err:%d\n", ret);
      }
      lua_pushnil(L);
      return 1;
    }
    while (1) {
      ret = xwii_iface_dispatch(devh[num-1].iface, &event, sizeof(event));
      if (ret) {
	if (ret != -EAGAIN) {
	  fprintf(stderr, "xwii_poll: read failed err:%d\n", ret);
	}
	break;
      } else {
	switch (event.type) {
	// key events:
	case XWII_EVENT_KEY:
	case XWII_EVENT_CLASSIC_CONTROLLER_KEY:
	case XWII_EVENT_PRO_CONTROLLER_KEY:
	case XWII_EVENT_NUNCHUK_KEY:
	case XWII_EVENT_DRUMS_KEY:
	case XWII_EVENT_GUITAR_KEY:
	  {
	    int i = 0;
	    lua_newtable(L);
	    lua_pushinteger(L, ++i);
	    lua_pushinteger(L, event.v.key.code);
	    lua_settable(L, -3);
	    lua_pushinteger(L, ++i);
	    lua_pushinteger(L, event.v.key.state);
	    lua_settable(L, -3);
	    return 1;
	  }
	// hotplug events:
	case XWII_EVENT_WATCH:
	  {
	    int ret = xwii_iface_open(devh[num-1].iface,
				      xwii_iface_available(devh[num-1].iface));
	    if (ret)
	      fprintf(stderr, "xwii_poll: cannot open interface #%d err: %d\n",
		      num, ret);
	    else
	      fprintf(stderr, "xwii_poll: hotplug event on interface #%d\n",
		      num);
	    break;
	  }
	// this is sent when the device was removed:
	case XWII_EVENT_GONE:
	  devh[num-1].fds[0].fd = -1;
	  devh[num-1].fds[0].events = 0;
	  devh[num-1].fds_num = 0;
	  fprintf(stderr, "xwii_poll: device #%d was removed\n", num);
	  lua_pushinteger(L, event.type);
	  return 1;
	// motion events:
	case XWII_EVENT_ACCEL:
	  devh[num-1].accel = event.v.abs[0];
	  break;
	case XWII_EVENT_IR:
	  {
	    int i;
	    for (i = 0; i < 4; i++)
	      devh[num-1].ir[i] = event.v.abs[i];
	    break;
	  }
	case XWII_EVENT_BALANCE_BOARD:
	  {
	    int i;
	    for (i = 0; i < 4; i++)
	      devh[num-1].board[i] = event.v.abs[i];
	    break;
	  }
	case XWII_EVENT_CLASSIC_CONTROLLER_MOVE:
	case XWII_EVENT_PRO_CONTROLLER_MOVE:
	  // XXFIXME: We store the joystick positions of both the classic and
	  // pro controllers in the same field for now. Maybe these should be
	  // split up?
	  devh[num-1].pro[0] = event.v.abs[0];
	  devh[num-1].pro[1] = event.v.abs[1];
	  break;
	case XWII_EVENT_MOTION_PLUS:
	  devh[num-1].motion = event.v.abs[0];
	  break;
	case XWII_EVENT_NUNCHUK_MOVE:
	  devh[num-1].nunchuk_accel = event.v.abs[1];
	  devh[num-1].nunchuk_stick = event.v.abs[0];
	  break;
	// ignore everything else; XXXTODO: guitar and drum movements
	default:
	  //fprintf(stderr, "xwii_poll: unrecognized event #%d\n", event.type);
	  break;
	}
      }
    }
  }
  lua_pushnil(L);
  return 1;
}

// The following functions return the current movement data from the various
// input devices as a single table. In most cases, the table contains the
// corresponding x, y and z values (just x and y for IR and the Classic/Pro
// Controller and Nunchuk sticks). The IR camera can track up to four IR
// sources, so the resulting table contains four pairs of x, y values (eight
// values in total). Likewise, two pairs of x, y data are returned for the
// Classic/Pro Controller's two joystick positions. For the balance board, the
// table contains four weight values, one for each of the edges of the
// board. Please note that all this data is updated by xwii_poll, so that
// function must be called beforehand to get current values.

static void push_xyz(lua_State *L, struct xwii_event_abs *abs)
{
  int i = 0;
  lua_newtable(L);
  lua_pushinteger(L, ++i);
  lua_pushinteger(L, abs->x);
  lua_settable(L, -3);
  lua_pushinteger(L, ++i);
  lua_pushinteger(L, abs->y);
  lua_settable(L, -3);
  lua_pushinteger(L, ++i);
  lua_pushinteger(L, abs->z);
  lua_settable(L, -3);
}

static void push_xy(lua_State *L, struct xwii_event_abs *abs)
{
  int i = 0;
  lua_newtable(L);
  lua_pushinteger(L, ++i);
  lua_pushinteger(L, abs->x);
  lua_settable(L, -3);
  lua_pushinteger(L, ++i);
  lua_pushinteger(L, abs->y);
  lua_settable(L, -3);
}

// Core input devices (accelerometer and IR tracker)
static int l_xwii_accel(lua_State *L)
{
  int num = (int)luaL_checknumber(L, 1);
  if (num >= 1 && num <= NDEV && devh[num-1].fds_num &&
      (xwii_iface_opened(devh[num-1].iface) & XWII_IFACE_CORE)) {
    push_xyz(L, &devh[num-1].accel);
  } else {
    lua_pushnil(L);
  }
  return 1;
}

static int l_xwii_ir(lua_State *L)
{
  int num = (int)luaL_checknumber(L, 1);
  if (num >= 1 && num <= NDEV && devh[num-1].fds_num &&
      (xwii_iface_opened(devh[num-1].iface) & XWII_IFACE_CORE)) {
    int i;
    lua_newtable(L);
    for (i = 0; i < 4; i++) {
      lua_pushinteger(L, 2*i+1);
      lua_pushinteger(L, devh[num-1].ir[i].x);
      lua_settable(L, -3);
      lua_pushinteger(L, 2*i+2);
      lua_pushinteger(L, devh[num-1].ir[i].y);
      lua_settable(L, -3);
    }
  } else {
    lua_pushnil(L);
  }
  return 1;
}

// This requires Motion-Plus (built into the Wii Remote Plus, also available
// as an extension).
static int l_xwii_motion_plus(lua_State *L)
{
  int num = (int)luaL_checknumber(L, 1);
  if (num >= 1 && num <= NDEV && devh[num-1].fds_num &&
      (xwii_iface_opened(devh[num-1].iface) & XWII_IFACE_MOTION_PLUS)) {
    push_xyz(L, &devh[num-1].motion);
  } else {
    lua_pushnil(L);
  }
  return 1;
}

// The following require the Nunchuk.
static int l_xwii_nunchuk_accel(lua_State *L)
{
  int num = (int)luaL_checknumber(L, 1);
  if (num >= 1 && num <= NDEV && devh[num-1].fds_num &&
      (xwii_iface_opened(devh[num-1].iface) & XWII_IFACE_NUNCHUK)) {
    push_xyz(L, &devh[num-1].nunchuk_accel);
  } else {
    lua_pushnil(L);
  }
  return 1;
}

static int l_xwii_nunchuk_stick(lua_State *L)
{
  int num = (int)luaL_checknumber(L, 1);
  if (num >= 1 && num <= NDEV && devh[num-1].fds_num &&
      (xwii_iface_opened(devh[num-1].iface) & XWII_IFACE_NUNCHUK)) {
    push_xy(L, &devh[num-1].nunchuk_stick);
  } else {
    lua_pushnil(L);
  }
  return 1;
}

// This requires the Classic/Pro Controller.
static int l_xwii_pro_stick(lua_State *L)
{
  int num = (int)luaL_checknumber(L, 1);
  if (num >= 1 && num <= NDEV && devh[num-1].fds_num &&
      (xwii_iface_opened(devh[num-1].iface) & XWII_IFACE_CORE)) {
    int i;
    lua_newtable(L);
    for (i = 0; i < 2; i++) {
      lua_pushinteger(L, 2*i+1);
      lua_pushinteger(L, devh[num-1].pro[i].x);
      lua_settable(L, -3);
      lua_pushinteger(L, 2*i+2);
      lua_pushinteger(L, devh[num-1].pro[i].y);
      lua_settable(L, -3);
    }
  } else {
    lua_pushnil(L);
  }
  return 1;
}

// This requires the Balance Board.
static int l_xwii_board(lua_State *L)
{
  int num = (int)luaL_checknumber(L, 1);
  if (num >= 1 && num <= NDEV && devh[num-1].fds_num &&
      (xwii_iface_opened(devh[num-1].iface) & XWII_IFACE_CORE)) {
    int i;
    lua_newtable(L);
    for (i = 0; i < 4; i++) {
      lua_pushinteger(L, i+1);
      lua_pushinteger(L, devh[num-1].board[i].x);
      lua_settable(L, -3);
    }
  } else {
    lua_pushnil(L);
  }
  return 1;
}

static const struct luaL_Reg xwiilua [] = {
  {"xwii_list", l_xwii_list},
  {"xwii_open", l_xwii_open},
  {"xwii_close", l_xwii_close},
  {"xwii_info", l_xwii_info},
  {"xwii_get_battery", l_xwii_get_battery},
  {"xwii_get_leds", l_xwii_get_leds},
  {"xwii_set_leds", l_xwii_set_leds},
  {"xwii_rumble", l_xwii_rumble},
  {"xwii_poll", l_xwii_poll},
  {"xwii_accel", l_xwii_accel},
  {"xwii_ir", l_xwii_ir},
  {"xwii_motion_plus", l_xwii_motion_plus},
  {"xwii_nunchuk_accel", l_xwii_nunchuk_accel},
  {"xwii_nunchuk_stick", l_xwii_nunchuk_stick},
  {"xwii_pro_stick", l_xwii_pro_stick},
  {"xwii_board", l_xwii_board},
  {NULL, NULL}  /* sentinel */
};

int luaopen_xwiilua (lua_State *L) {
  luaL_newlib(L, xwiilua);
  return 1;
}
