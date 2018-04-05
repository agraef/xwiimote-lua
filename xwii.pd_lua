
-- Easy access to the Wii Remote from Pd. The object allows you to query the
-- list of known devices, open a device, poll for key events and query
-- movement (x, y, z) data. The usual extensions such as Motion-Plus and the
-- Nunchuk are also supported. Please check the accompanying help patch for an
-- example showing how this external is used.

-- Once the object is kicked off with a bang or a nonzero number, the device
-- is polled at regular intervals. The object outputs key events on the first
-- outlet when they occur. Special messages can be used to retrieve movement
-- data and other information about the device on the second outlet. A zero on
-- the inlet stops reporting and closes the device.

-- If you have more than one Wii Remote attached to your system, the number of
-- the device to be opened can be specified as the first creation argument.
-- By default, the first connected device will be used. Each device can be
-- opened only once, so you should have at most one xwii object for each
-- device in your patch.

-- The update period in msecs can be given as the second creation argument. If
-- not given then a hard-coded default of 10 msec is used, but you can change
-- this by setting the value of the self.period member below.

local xwii = pd.Class:new():register("xwii")

local xw = require("xwiilua")

function xwii:initialize(name, atoms)
   self.inlets = 1
   self.outlets = 2
   -- first arg is device number (1 by default)
   self.dev = #atoms>0 and atoms[1] or nil
   if self.dev == nil then
      self.dev = 1
   end
   if type(self.dev) ~= "number" or self.dev < 1 or
   self.dev ~= math.floor(self.dev) then
      pd.post("xwii: error: device number must be a positive integer")
      return false
   end
   -- second arg is poll interval (10 msecs by default)
   self.period = #atoms>1 and atoms[2] or nil
   if self.period == nil then
      self.period = 10
   end
   if type(self.period) ~= "number" or self.period < 1 or
   self.period ~= math.floor(self.period) then
      pd.post("xwii: error: poll interval must be a positive integer")
      return false
   end
   -- device isn't opened at initialization time
   self.d = 0
   return true
end

function xwii:postinitialize()
   self.clock = pd.Clock:new():register(self, "tick")
end

function xwii:finalize()
   self.clock:destruct()
   xw.xwii_close(self.d)
end

-- The devlist message outputs the list of all connected devices as a list of
-- symbols on the second outlet (each entry is the corresponding device
-- path). This always works (the xwii object doesn't have to be associated
-- with a device yet).
function xwii:in_1_devlist()
   local t = xw.xwii_list()
   if #t > 0 then
      self:outlet(2, "devlist", t)
   end
end

-- The following require that the device has been opened already.

-- Output the interface type bitmask of the opened device on the second outlet
-- (see xwii_iface_type in the xwiimote.h header file for possible values).
function xwii:in_1_info()
   if self.d > 0 then
      local f = xw.xwii_info(self.d)
      self:outlet(2, "info", {f})
   end
end

-- Retrieve the battery status.
function xwii:in_1_battery()
   if self.d > 0 then
      local f = xw.xwii_get_battery(self.d)
      self:outlet(2, "battery", {f})
   end
end

-- Read or write the status of the 4 leds to/from a bitmask (lsb is leftmost
-- led). Current status is retrieved as a non-negative integer if no arguments
-- are given, otherwise the (single) argument must be a non-negative integer.
function xwii:in_1_leds(args)
   if self.d > 0 then
      if #args == 0 then
	 local f = xw.xwii_get_leds(self.d)
	 self:outlet(2, "leds", {f})
      elseif #args > 1 then
	 self:error("xwii: leds: expected a single integer argument")
      else
	 local f = args[1]
	 if type(f) ~= "number" or f < 0 or f ~= math.floor(f) then
	    self:error("xwii: leds: argument must be a non-negative integer")
	 else
	    xw.xwii_set_leds(self.d, f)
	 end
      end
   end
end

-- Start and stop the rumble motor. The argument must be a non-negative
-- integer (zero denotes off, non-zero on).
function xwii:in_1_rumble(args)
   if self.d > 0 then
      if #args ~= 1 then
	 self:error("xwii: rumble: expected a single integer argument")
      else
	 local f = args[1]
	 if type(f) ~= "number" or f < 0 or f ~= math.floor(f) then
	    self:error("xwii: rumble: argument must be a non-negative integer")
	 else
	    xw.xwii_rumble(self.d, f)
	 end
      end
   end
end

-- Report the current movement data for various kinds of devices on the second
-- outlet. The result is a list of coordinates as reported by the
-- corresponding device (normally x, y, z, or x, y for IR tracking and
-- joysticks) with the input symbol (accel, ir etc.) as a selector before it.

-- The accelerometer (x, y, z).
function xwii:in_1_accel()
   local t = xw.xwii_accel(self.d)
   if t ~= nil then
      self:outlet(2, "accel", t)
   end
end

-- This outputs 8 values (x, y for up to four IR traces).
function xwii:in_1_ir()
   local t = xw.xwii_ir(self.d)
   if t ~= nil then
      self:outlet(2, "ir", t)
   end
end

-- This reports velocity instead of acceleration values. Needs Motion-Plus.
function xwii:in_1_motionplus()
   local t = xw.xwii_motion_plus(self.d)
   if t ~= nil then
      self:outlet(2, "motionplus", t)
   end
end

-- The Nunchuk's accelerometer (x, y, z).
function xwii:in_1_ncaccel()
   local t = xw.xwii_nunchuk_accel(self.d)
   if t ~= nil then
      self:outlet(2, "ncaccel", t)
   end
end

-- The Nunchuk's joystick (x, y).
function xwii:in_1_ncstick()
   local t = xw.xwii_nunchuk_stick(self.d)
   if t ~= nil then
      self:outlet(2, "ncstick", t)
   end
end

-- The Classic/Pro Controller's two joysticks (x, y for each, so four values
-- in total).
function xwii:in_1_prostick()
   local t = xw.xwii_pro_stick(self.d)
   if t ~= nil then
      self:outlet(2, "prostick", t)
   end
end

-- The Balance Board (4 weight values, one for each edge of the board).
function xwii:in_1_board()
   local t = xw.xwii_pro_stick(self.d)
   if t ~= nil then
      self:outlet(2, "board", t)
   end
end

-- Open the device and start polling for key events.
function xwii:in_1_bang()
   self.d = xw.xwii_open(self.dev)
   self:tick()
end

-- Open (f=1) or close (f=0) the device.
function xwii:in_1_float(f)
   if f ~= 0 then
      self.d = xw.xwii_open(self.dev)
      self:tick()
   else
      self.clock:unset()
      xw.xwii_close(self.d)
      self.d = 0
   end
end

-- Clock tick, polls for available key events and outputs them on the first
-- outlet. Each key event is given as a list of two numbers: The key code (0 =
-- left, 1 = right, 2 = up, 3 = down, 4 = A, 5 = B, etc.; please check the
-- xwii_event_keys type in the xwiimote.h header file for possible values) and
-- the key status (1 if the button is pressed, 0 if it is released).
function xwii:tick()
   while self.d > 0 do
      local ev = xw.xwii_poll(self.d)
      if ev ~= nil then
	 self:outlet(1, "list", ev)
      else
	 break
      end
   end
   self.clock:delay(self.period)
end
