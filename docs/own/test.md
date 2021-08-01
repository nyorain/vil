# Things we should test

Unit tests:
- useful error messages for the buffer parser.
  just look how it's done with pegtl for their json parser, could do
  it like that.

UI:
We should likely build an application that just does every command at
least once, even if non-sensical.
- all transfer commands in command viewer. The logic there is kinda ugly
  and some commands were never tested.
