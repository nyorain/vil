Debug utils label nesting
=========================

Vulkan allows applications to nest debug utils labels in *any* way with
other commands. A debug label can start outside of a render pass and end within
in or the other way around. Since we need to represent commands as a hierarchy
(think e.g. of the command viewer GUI, without this hierarchy we wouldn't
be able to collapse render passes or labels) this is a problem.
How should a command buffer record with such a command look in the gui?!

We currently fix this by just fixing the hierarchy we build internally, see
the various extra tracking and workarounds in CommandBuffer, during recording.
