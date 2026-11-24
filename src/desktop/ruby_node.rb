require "base64"
require "json"
require "stringio"

class RubyEditConnector
  include Comparable
  attr_reader :index, :name

  def initialize(index, name)
    @index, @name = index, name
  end

  def <=>(other)
    case other
    when String, Symbol then name <=> other.to_s
    when RubyEditConnector then index <=> other.index
    else index <=> other
    end
  end

  def coerce(other)
    [other, index]
  end

  def to_int
    index
  end
  alias to_i to_int

  def to_str
    name
  end
  alias to_s to_str

  def +(other)
    index + other
  end

  def -(other)
    index - other
  end
end

class MIDI
  attr_reader :openstone_bytes

  def initialize(*fields)
    case fields.size
    when 1
      hex = String(fields.first).delete(" \t\r\n")
      unless hex.match?(/\AF0(?:[0-7][0-9A-F])+F7\z/i)
        raise ArgumentError, "Expected a complete MIDI SysEx hex string"
      end
      @openstone_bytes = [hex].pack("H*").freeze
    when 4
      status, channel, data1, data2 = fields
      unless fields.all? { |field| field.is_a?(Integer) } &&
             status.between?(0x80, 0xef) && channel.between?(1, 16) &&
             data1.between?(0, 127) && data2.between?(0, 127)
        raise ArgumentError, "Expected a MIDI channel status, channel 1 through 16, and 7-bit data"
      end
      status &= 0xf0
      bytes = [status | (channel - 1), data1]
      bytes << data2 unless status == 0xc0 || status == 0xd0
      @openstone_bytes = bytes.pack("C*").freeze
    else
      raise ArgumentError, "MIDI.new expects four channel message fields or a SysEx hex string"
    end
  end

  def to_array
    status, data1, data2 = @openstone_bytes.byteslice(0, 3).bytes
    if status == 0xf0
      [@openstone_bytes.unpack1("H*").upcase, @openstone_bytes.bytesize]
    elsif status < 0xf0
      [status & 0xf0, (status & 0x0f) + 1, data1 || 0, data2 || 0]
    else
      [status, 0, data1 || 0, data2 || 0]
    end
  end

  def self.openstone_from_bytes(bytes)
    message = allocate
    message.instance_variable_set(:@openstone_bytes, bytes.freeze)
    message
  end
end

class Color
  attr_reader :argb

  def initialize(*components)
    channels = case components.length
               when 1 then [255, components[0], components[0], components[0]]
               when 3 then [255, *components]
               when 4 then components
               else raise ArgumentError, "Color expects gray, RGB, or ARGB channels"
               end
    @argb = channels.reduce(0) do |packed, component|
      channel = Integer(component)
      raise ArgumentError, "Color channels must be between 0 and 255" unless (0..255).include?(channel)
      (packed << 8) | channel
    end
  end
end

class Bitmap
  attr_reader :widthPixels, :heightPixels, :openstone_reference

  def initialize(reference, pixel_width, pixel_height)
    @openstone_reference = reference
    @widthPixels = pixel_width
    @heightPixels = pixel_height
  end

  def width
    @widthPixels / 8.0
  end

  def height
    @heightPixels / 8.0
  end
end

class Font
  def initialize(face, size, style = "normal")
    @face = String(face)
    @size = Float(size)
    unless @size.finite? && @size.positive?
      raise ArgumentError, "Font size must be positive and finite"
    end
    unless style.is_a?(String) || (style.is_a?(Integer) && (0..15).include?(style))
      raise ArgumentError, "Font style must be a string or an integer from 0 to 15"
    end
    @style = style
  end

  def openstone_description
    [@face.dup, @size, @style.is_a?(String) ? @style.dup : @style]
  end
end

class StringFormat
  def initialize
    @alignment = @line_alignment = "near"
    @flags = 0
  end

  def setAlignment(alignment)
    @alignment = openstone_alignment(alignment)
  end

  def setLineAlignment(alignment)
    @line_alignment = openstone_alignment(alignment)
  end

  def setFlags(flags)
    unless flags.is_a?(Integer) && (0..0x7fff_ffff).include?(flags)
      raise ArgumentError, "StringFormat flags must be a nonnegative integer bitmask"
    end
    @flags = flags
  end

  def openstone_description
    [@alignment.dup, @line_alignment.dup, @flags]
  end

  private

  def openstone_alignment(alignment)
    name = alignment.to_s.strip.downcase
    raise ArgumentError, "Expected near, center, or far" unless %w[near center far].include?(name)
    name
  end
end

class Brush
  attr_reader :color

  def initialize(color)
    setColor(color)
  end

  def setColor(color)
    raise TypeError, "Expected a Color" unless color.is_a?(Color)
    @color = color
  end
end

class Pen
  attr_reader :color, :dash_style

  def initialize(color, width)
    setColor(color)
    setWidth(width)
    @dash_style = "solid"
  end

  def setDashStyle(style)
    @dash_style = String(style).dup
  end

  def setColor(color)
    raise TypeError, "Expected a Color" unless color.is_a?(Color)
    @color = color
  end

  def getWidth
    @width
  end

  def setWidth(width)
    width = Float(width)
    raise ArgumentError, "Pen width must be finite and nonnegative" unless width.finite? && width >= 0
    @width = width
  end
end

class View
  attr_reader :width, :height

  def initialize(width, height, commands, &measure_text)
    @width, @height, @commands = width, height, commands
    @measure_text = measure_text
  end

  def drawRectangle(instrument, rect)
    openstone_shape("rectangle", instrument, rect)
  end

  def drawEllipse(instrument, rect)
    openstone_shape("ellipse", instrument, rect)
  end

  def drawRoundRect(instrument, rect, corner)
    corner = Float(corner)
    raise ArgumentError, "Corner radius must be finite" unless corner.finite?
    openstone_shape("roundRect", instrument, rect, corner)
  end

  def drawLine(instrument, point1, point2)
    drawLines(instrument, [point1, point2])
  end

  def drawLines(instrument, points)
    raise TypeError, "Expected a Pen" unless instrument.is_a?(Pen)
    openstone_path("lines", instrument, points, 2)
  end

  def drawPolygon(instrument, points)
    openstone_path("polygon", instrument, points, 3)
  end

  def drawBitmap(bitmap, location, alpha = 255)
    raise TypeError, "Expected a Bitmap" unless bitmap.is_a?(Bitmap)
    point = location.is_a?(Array) && location.length == 2
    bounds = openstone_coordinates(location, point ? 2 : 4, "a point or rectangle")
    bounds += [bitmap.width, bitmap.height] if point
    opacity = Integer(alpha)
    raise ArgumentError, "Bitmap alpha must be between 0 and 255" unless (0..255).include?(opacity)
    return if bitmap.widthPixels.zero? || bounds[2].zero? || bounds[3].zero?
    @commands << ["bitmap", bitmap.openstone_reference, bounds, opacity]
    nil
  end

  def drawString(text, font, format, location, brush)
    raise TypeError, "Expected a Brush" unless brush.is_a?(Brush)
    description = openstone_text_description(text, font, format, location)
    bounds, label = description
    return if label.empty? || (bounds.length == 4 && (bounds[2].zero? || bounds[3].zero?))

    @commands << ["text", description, brush.color.argb]
    nil
  end

  def measureString(text, font, format, location)
    raise RuntimeError, "measureString requires an active drawing View" unless @measure_text
    @measure_text.call(openstone_text_description(text, font, format, location))
  end

  private

  def openstone_text_description(text, font, format, location)
    raise TypeError, "Expected a Font" unless font.is_a?(Font)
    raise TypeError, "Expected a StringFormat or nil" unless format.nil? || format.is_a?(StringFormat)
    point = location.is_a?(Array) && location.length == 2
    bounds = openstone_coordinates(location, point ? 2 : 4, "a point or rectangle")
    [bounds, String(text).dup, font.openstone_description, format&.openstone_description]
  end

  def openstone_shape(kind, instrument, rect, *options)
    bounds = openstone_coordinates(rect, 4, "a rectangle with x, y, width, and height")
    return if bounds[2].zero? || bounds[3].zero?
    openstone_drawing(kind, bounds, instrument, *options)
  end

  def openstone_path(kind, instrument, points, minimum)
    raise ArgumentError, "Expected an array of points" unless points.is_a?(Array)
    vertices = points.map { |point| openstone_coordinates(point, 2, "a point with x and y") }
    return if vertices.uniq.length < minimum
    openstone_drawing(kind, vertices, instrument)
  end

  def openstone_coordinates(values, size, description)
    unless values.is_a?(Array) && values.length == size
      raise ArgumentError, "Expected #{description}"
    end
    coordinates = values.map { |value| Float(value) }
    raise ArgumentError, "Shape coordinates must be finite" unless coordinates.all?(&:finite?)
    coordinates
  end

  def openstone_drawing(kind, geometry, instrument, *options)
    unless instrument.is_a?(Pen) || instrument.is_a?(Brush)
      raise TypeError, "Expected a Pen or Brush"
    end
    pen = instrument.is_a?(Pen) ? [instrument.getWidth, instrument.dash_style.dup] : nil
    @commands << [kind, geometry, instrument.color.argb, pen, *options]
    nil
  end
end

class RubyEdit
  def initialize(request, protocol)
    @openstone_protocol = protocol
    @openstone_id = request.fetch("id")
    @openstone_location = request.fetch("schematicLocation")
    @this = self
    @openstone_source = request.fetch("source")
    @openstone_input_labels = request.fetch("inputs")
    @openstone_output_labels = request.fetch("outputs")
    @openstone_triggers = request.fetch("triggers")
    @openstone_input_triggers = request.fetch("inputTriggers")
    @openstone_view_inputs = request.fetch("viewInputs")
    @openstone_midi_inputs = request.fetch("midiInputs")
    @openstone_midi_outputs = request.fetch("midiOutputs")
    @openstone_ruby_inputs = request.fetch("rubyInputs")
    @openstone_ruby_outputs = request.fetch("rubyOutputs")
    @openstone_float_inputs = request.fetch("floatInputs")
    @openstone_float_array_inputs = request.fetch("floatArrayInputs")
    @openstone_float_outputs = request.fetch("floatOutputs")
    @openstone_float_array_outputs = request.fetch("floatArrayOutputs")
    @ins = request.fetch("values")
    @outs = Array.new(@openstone_output_labels.length)
    @openstone_input_labels.each_index { |i| openstone_remember_input(i, @ins[i]) }
  end

  def output(*args)
    port, value, timestamp = openstone_event_arguments(@openstone_output_labels, args)
    openstone_post("output", port, value, @openstone_ruby_outputs[port], @openstone_triggers[port], timestamp)
    @outs[port] = value unless timestamp
    value
  end

  def input(*args)
    port, value, timestamp = openstone_event_arguments(@openstone_input_labels, args)
    openstone_post("input", port, value, @openstone_ruby_inputs[port], @openstone_input_triggers[port], timestamp)
  end

  def time
    @openstone_time
  end

  def getViewSize(connector = 0)
    return nil if @openstone_view_inputs.empty?
    port = openstone_connector_index(@openstone_input_labels, connector)
    return nil unless @openstone_view_inputs[port]
    openstone_host_call("getViewSize", "port" => port)["result"]
  end

  def isKeyPressed(key)
    openstone_host_call("isKeyPressed", "key" => key.to_s).fetch("result")
  end

  def schematicLocation
    @openstone_location.dup
  end

  def openstone_update_location(location)
    @openstone_location = location
  end

  def scheduleMethod(name, *arguments)
    unless (name.is_a?(String) || name.is_a?(Symbol)) && !arguments.empty?
      raise ArgumentError, "scheduleMethod expects a method name, optional arguments, and a timestamp"
    end
    timestamp = openstone_timestamp(arguments.pop)
    openstone_queue(["method", name.to_s, arguments, true, timestamp])
    nil
  end

  def clearEvents
    openstone_queue(["clear"])
    nil
  end

  def redraw(connector = nil, area = nil)
    if connector.is_a?(Array) && area.nil?
      area = connector
      connector = nil
    end
    port = connector.nil? ? @openstone_view_inputs.index(true) : openstone_connector_index(@openstone_input_labels, connector)
    return unless port
    raise ArgumentError, "Redraw expects a View input" unless @openstone_view_inputs[port]

    unless area.nil?
      unless area.is_a?(Array) && area.length == 4
        raise ArgumentError, "Expected a redraw rectangle with x, y, width, and height"
      end
      area = area.map { |coordinate| Float(coordinate) }
      raise ArgumentError, "Redraw coordinates must be finite" unless area.all?(&:finite?)
    end
    openstone_queue(["redraw", port, area])
    nil
  end

  def captureMouse
    unless %w[mouseLDown mouseRDown].include?(@openstone_mouse_callback)
      raise RuntimeError, "captureMouse belongs in a mouse down callback"
    end
    @openstone_mouse_capture = true
    nil
  end

  def releaseMouse
    raise RuntimeError, "releaseMouse belongs in a mouse callback" unless @openstone_mouse_callback
    @openstone_mouse_capture = false
    nil
  end

  def showCursor(show)
    raise ArgumentError, "showCursor expects true or false" unless show.equal?(true) || show.equal?(false)
    openstone_queue(["showCursor", show])
    nil
  end

  def createEdit(connector, id, area, text = nil, font = nil, text_color = nil, background = nil, multiline = false)
    port = openstone_connector_index(@openstone_input_labels, connector)
    raise ArgumentError, "createEdit expects a View input" unless @openstone_view_inputs[port]
    unless area.is_a?(Array) && area.length == 4
      raise ArgumentError, "Expected an edit rectangle with x, y, width, and height"
    end
    bounds = area.map { |coordinate| Float(coordinate) }
    unless bounds.all?(&:finite?) && bounds[2].positive? && bounds[3].positive?
      raise ArgumentError, "Edit bounds must be finite and have positive width and height"
    end
    raise TypeError, "Expected a Font" unless font.nil? || font.is_a?(Font)
    unless [text_color, background].all? { |color| color.nil? || color.is_a?(Color) }
      raise TypeError, "Expected a Color"
    end
    options = { "area" => bounds, "text" => String(text).dup, "font" => font&.openstone_description,
                "color" => text_color&.argb, "back" => background&.argb, "multiline" => !!multiline }
    openstone_queue(["createEdit", port, id, true, options])
    nil
  end

  def createDropList(connector, id, position, items, checked = nil, style = nil, disabled = nil)
    port = openstone_connector_index(@openstone_input_labels, connector)
    raise ArgumentError, "createDropList expects a View input" unless @openstone_view_inputs[port]
    unless position.is_a?(Array) && position.length == 2
      raise ArgumentError, "Expected a drop list position with x and y"
    end
    anchor = position.map { |coordinate| Float(coordinate) }
    raise ArgumentError, "Drop list coordinates must be finite" unless anchor.all?(&:finite?)
    options = { "position" => anchor, "items" => String(items).dup, "style" => String(style).dup,
                "checked" => checked.nil? ? -1 : Integer(checked), "disabled" => disabled.nil? ? -1 : Integer(disabled) }
    openstone_queue(["createDropList", port, id, true, options])
    nil
  end

  def messageBox(id, text, title = nil, type = nil)
    options = { "text" => String(text).dup, "title" => String(title).dup, "type" => String(type).dup }
    openstone_queue(["messageBox", 0, id, true, options])
    nil
  end

  def playSound(file, mode = "async")
    unless @openstone_actions && Thread.current == Thread.main && !@openstone_drawing && !@openstone_hit_testing
      raise RuntimeError, "playSound requires a Ruby event or initialization callback"
    end
    mode = String(mode).downcase
    unless file.nil? || %w[async sync loop].include?(mode)
      raise ArgumentError, "playSound expects async, sync, or loop"
    end
    openstone_host_call("playSound", "file" => file.nil? ? nil : String(file), "mode" => mode)
    nil
  end

  def watch(*values)
    raise ArgumentError, "watch expects a value or a label and value" unless (1..2).include?(values.length)
    name, value = values.size == 1 ? ["Quick Watch", values[0]] : values
    @openstone_watches[name] = openstone_inspect(value)
    nil
  end

  def openstone_start(time, state)
    openstone_capture(time) do
      result = instance_eval(@openstone_source, "Ruby node", 1)
      init if respond_to?(:init, true)
      loadState(Marshal.load(Base64.strict_decode64(state))) if state && respond_to?(:loadState, true)
      result
    end
  end

  def openstone_save_state(time)
    state = nil
    if respond_to?(:saveState, true)
      openstone_capture(time) do
        state = Base64.strict_encode64(Marshal.dump(saveState))
        nil
      end
    end
    { "state" => state, "actions" => [] }
  end

  def openstone_call_method(name, arguments, time)
    openstone_capture(time) { method(name).call(*arguments) }
  end

  def openstone_draw(port, width, height, time)
    commands = []
    if respond_to?(:draw, true)
      @openstone_drawing = true
      openstone_capture(time) do
        view = View.new(width, height, commands) do |description|
          unless @openstone_drawing && Thread.current == Thread.main
            raise RuntimeError, "measureString requires an active drawing View"
          end
          openstone_host_call("measureString", "text" => description).fetch("result")
        end
        handler = method(:draw)
        positional = handler.parameters.count { |kind, _| kind == :req || kind == :opt }
        if positional >= 2
          handler.call(RubyEditConnector.new(port, @openstone_input_labels[port]), view)
        else
          handler.call(view)
        end
        nil
      end
    end
    { "drawings" => commands, "actions" => [] }
  ensure
    @openstone_drawing = false
  end

  def openstone_mouse_hit(port, x, y, time, query_cursor)
    hit = false
    cursor = nil
    @openstone_hit_testing = true
    if respond_to?(:isInMousePoint, true)
      openstone_capture(time) do
        result = openstone_mouse_call(:isInMousePoint, port, x, y)
        hit = result == true || result == 1
        if hit && query_cursor && respond_to?(:mouseCursor, true)
          cursor = openstone_mouse_cursor(port, x, y)
        end
        nil
      end
    end
    { "hit" => hit, "cursor" => cursor, "actions" => [] }
  ensure
    @openstone_hit_testing = false
  end

  def openstone_mouse_event(name, port, x, y, time)
    if %w[mouseLUpCaptured mouseRUpCaptured].include?(name) && !respond_to?(name, true)
      name = name.delete_suffix("Captured")
    end
    return { "actions" => [] } unless respond_to?(name, true)
    @openstone_mouse_callback = name
    @openstone_mouse_capture = nil
    response = openstone_capture(time) { openstone_mouse_call(name, port, x, y) }
    response.merge("called" => true, "captureMouse" => @openstone_mouse_capture)
  ensure
    @openstone_mouse_callback = nil
    @openstone_mouse_capture = nil
  end

  def openstone_edit_event(finished, id, text, time)
    name = finished ? :editFinished : :editChanged
    return { "actions" => [] } unless respond_to?(name, true)
    openstone_capture(time) { method(name).call(id, text) }.merge("called" => true)
  end

  def openstone_drop_list_finished(id, index, name, time)
    return { "actions" => [] } unless respond_to?(:dropListFinished, true)
    openstone_capture(time) { dropListFinished(id, index, name) }.merge("called" => true)
  end

  def openstone_message_box_finished(id, result, time)
    return { "actions" => [] } unless respond_to?(:messageBoxFinished, true)
    openstone_capture(time) { messageBoxFinished(id, result) }.merge("called" => true)
  end

  def openstone_store_output(port, value)
    value = value.to_f if @openstone_float_outputs[port] && !value.nil?
    value = value.map(&:to_f) if @openstone_float_array_outputs[port] && !value.nil?
    @outs[port] = value
    { "result" => "", "actions" => [] }
  end

  def openstone_dispatch(i, value, time)
    value = openstone_remember_input(i, value)
    openstone_capture(time) do
      if respond_to?(:event, true)
        handler = method(:event)
        if handler.parameters.any? { |kind, _| kind == :rest }
          handler.call(RubyEditConnector.new(i, @openstone_input_labels[i]), value, time)
        else
          args = [RubyEditConnector.new(i, @openstone_input_labels[i]), value, time]
          count = handler.parameters.count { |kind, _| kind == :req || kind == :opt }
          handler.call(*args.take(count))
        end
      else
        instance_eval(@openstone_source, "Ruby node", 1)
      end
    end
  end

  private

  def openstone_host_call(method, arguments)
    raise RuntimeError, "#{method} requires a Ruby callback" unless @openstone_actions && Thread.current == Thread.main
    @openstone_protocol.puts(JSON.generate(arguments.merge("id" => @openstone_id, "hostCall" => method)))
    line = STDIN.gets
    raise RuntimeError, "#{method} host disconnected" unless line
    reply = JSON.parse(line)
    unless reply["id"] == @openstone_id && reply["hostReply"] == method
      raise RuntimeError, "Invalid #{method} reply"
    end
    raise RuntimeError, reply["error"] if reply.key?("error")
    reply
  end

  def openstone_mouse_cursor(port, x, y)
    cursor = method(:mouseCursor).arity == 0 ? mouseCursor : openstone_mouse_call(:mouseCursor, port, x, y)
    cursor.to_s if cursor.is_a?(Integer) || cursor.is_a?(String)
  rescue StandardError
    nil
  end

  def openstone_mouse_call(name, port, x, y)
    handler = method(name)
    positional = handler.parameters.count { |kind, _| kind == :req || kind == :opt }
    args = [Float(x), Float(y)]
    args.unshift(RubyEditConnector.new(port, @openstone_input_labels[port])) if positional >= 3
    handler.call(*args)
  end

  def openstone_event_arguments(labels, args)
    unless (1..3).include?(args.length)
      raise ArgumentError, "Expected a value or a connector, value, and optional timestamp"
    end
    port, value, stamp = args.size == 1 ? [0, args[0], nil] : args
    [openstone_connector_index(labels, port), value, stamp.nil? ? nil : openstone_timestamp(stamp)]
  end

  def openstone_connector_index(labels, port)
    port = labels.index(port.to_s) if port.is_a?(String) || port.is_a?(Symbol)
    port = port.to_int if port.is_a?(RubyEditConnector)
    unless port.is_a?(Integer) && port >= 0 && port < labels.length
      raise ArgumentError, "Unknown Ruby connector"
    end
    port
  end

  def openstone_timestamp(stamp)
    value = Float(stamp)
    raise ArgumentError, "Ruby event time must be finite" unless value.finite?
    value
  end

  def openstone_post(direction, port, value, ruby_value, trigger, stamp)
    midi = direction == "output" ? @openstone_midi_outputs[port] : @openstone_midi_inputs[port]
    payload = if midi
                raise TypeError, "A MIDI connector expects a MIDI object" unless value.is_a?(MIDI)
                Base64.strict_encode64(value.openstone_bytes)
              elsif ruby_value
                value
              elsif trigger
                nil
              else
                openstone_snapshot(value)
              end
    openstone_queue([direction, port, payload, ruby_value, stamp])
    value
  end

  def openstone_queue(action)
    raise RuntimeError, "Event commands belong outside the draw callback" if @openstone_drawing
    raise RuntimeError, "Event commands belong outside mouse hit and cursor queries" if @openstone_hit_testing
    @openstone_actions << action
  end

  def openstone_remember_input(i, value)
    value = value.to_f if @openstone_float_inputs[i] && !value.nil?
    value = value.map(&:to_f) if @openstone_float_array_inputs[i] && !value.nil?
    @ins[i] = value
    @in = value if i == 0
    label = @openstone_input_labels[i]
    if label.match?(/\A[a-zA-Z_]\w*\z/) && !label.start_with?("openstone_") && !%w[in ins outs this].include?(label)
      instance_variable_set("@#{label}", value)
    end
    value
  end

  def openstone_snapshot(value)
    case value
    when Array then value.map { |item| item.is_a?(String) ? item.dup : item }
    when String then value.dup
    else value
    end
  end

  def openstone_inspect(value)
    String(value.inspect).encode("UTF-8", invalid: :replace, undef: :replace)
  rescue StandardError
    "<Ruby object>"
  end

  def openstone_capture(time)
    @openstone_time = time
    @openstone_actions = []
    @openstone_watches = {}
    printed = StringIO.new
    previous = $stdout
    $stdout = printed
    result = yield
    watches = @openstone_watches.map { |name, value| "#{name}: #{value}" }
    text = [printed.string.chomp, *watches, openstone_inspect(result)].reject(&:empty?).join("\n")
    { "result" => text, "actions" => @openstone_actions, "savesState" => respond_to?(:saveState, true),
      "drawsView" => respond_to?(:draw, true), "handlesMouse" => respond_to?(:isInMousePoint, true) }
  ensure
    $stdout = previous
    @openstone_actions = nil
  end
end

# Keep Ruby's STDOUT available to user scripts without mixing it with replies.
protocol = STDOUT.dup
protocol.sync = true
STDOUT.reopen(STDERR)

nodes = {}
objects = {}
next_reference = 0
bitmaps = ObjectSpace::WeakMap.new
resolve = lambda do |value|
  next value unless value.is_a?(Hash)
  if value.key?("midi")
    MIDI.openstone_from_bytes(Base64.strict_decode64(value.fetch("midi")))
  elsif value.key?("color")
    channels = [24, 16, 8, 0].map { |shift| (value.fetch("color") >> shift) & 255 }
    Color.new(*channels)
  elsif value.key?("bitmap")
    bitmap = Bitmap.new(value.fetch("bitmap"), value.fetch("width"), value.fetch("height"))
    bitmaps[bitmap] = bitmap
    bitmap
  else
    objects.fetch(value.fetch("rubyObject"))
  end
end

STDIN.each_line do |line|
  request = JSON.parse(line)
  id = request.fetch("id")
  case request["call"]
  when "drop"
    nodes.delete(id)
    next
  when "release"
    objects.delete(id)
    next
  end

  begin
    nodes[id]&.openstone_update_location(request.fetch("schematicLocation"))
    case request["call"]
    when "start"
      request["values"] = request.fetch("values").map(&resolve)
      node = RubyEdit.new(request, protocol)
      response = node.openstone_start(request.fetch("time"), request["state"])
      nodes[id] = node
    when "saveState"
      response = nodes.fetch(id).openstone_save_state(request.fetch("time"))
    when "event"
      response = nodes.fetch(id).openstone_dispatch(request.fetch("port"), resolve.call(request["value"]), request.fetch("time"))
    when "method"
      response = nodes.fetch(id).openstone_call_method(request.fetch("method"), resolve.call(request.fetch("arguments")),
                                                       request.fetch("time"))
    when "draw"
      response = nodes.fetch(id).openstone_draw(request.fetch("port"), request.fetch("width"), request.fetch("height"), request.fetch("time"))
    when "mouseHit"
      response = nodes.fetch(id).openstone_mouse_hit(request.fetch("port"), request.fetch("x"), request.fetch("y"),
                                                    request.fetch("time"), request.fetch("queryCursor"))
    when "mouseEvent"
      response = nodes.fetch(id).openstone_mouse_event(request.fetch("callback"), request.fetch("port"), request.fetch("x"),
                                                       request.fetch("y"), request.fetch("time"))
    when "editEvent"
      response = nodes.fetch(id).openstone_edit_event(request.fetch("finished"), resolve.call(request.fetch("editId")),
                                                       request.fetch("text"), request.fetch("time"))
    when "dropListFinished"
      response = nodes.fetch(id).openstone_drop_list_finished(resolve.call(request.fetch("listId")), request.fetch("index"),
                                                               request.fetch("name"), request.fetch("time"))
    when "messageBoxFinished"
      response = nodes.fetch(id).openstone_message_box_finished(resolve.call(request.fetch("messageId")),
                                                                 request.fetch("result"), request.fetch("time"))
    when "storeOutput"
      response = nodes.fetch(id).openstone_store_output(request.fetch("port"), resolve.call(request["value"]))
    else
      raise ArgumentError, "Unknown Ruby request"
    end
    new_objects = {}
    bitmap_references = Array(response["drawings"]).select { |drawing| drawing[0] == "bitmap" }.map { |drawing| drawing[1] }
    response["actions"].map! do |action|
      direction, target, value, ruby_value, stamp = action
      next action if %w[clear redraw showCursor].include?(direction)
      if ruby_value
        next_reference += 1
        new_objects[next_reference] = value
        value = next_reference
      elsif value.is_a?(Bitmap)
        bitmap_references << value.openstone_reference
        value = { "bitmap" => value.openstone_reference }
      elsif value.is_a?(Color)
        value = { "color" => value.argb }
      end
      [direction, target, value, stamp]
    end
    payload = JSON.generate(response.merge("id" => id, "objects" => new_objects.keys,
                                           "bitmaps" => (bitmap_references + bitmaps.keys.map(&:openstone_reference)).uniq))
    objects.merge!(new_objects)
  rescue Exception => error
    location = error.backtrace_locations&.find { |frame| frame.path == "Ruby node" }
    description = "#{error.class}: #{error.message}"
    description = "#{location}: #{description}" if location
    payload = JSON.generate("id" => id, "error" => description.encode("UTF-8", invalid: :replace, undef: :replace))
  end
  protocol.puts(payload)
end
