# problem

given 

- the selected command (full hierachy)
  probably rather as an abstract description (containing all information we need) rather
  the command itself since it may be invalid
- when in per-present-mode we also know its submission location,

we want to find the new command matching the selected one in a new submission (or in per-present-mode:
in the new list of submissions in that frame). We aren't just interested in the best match.
We always want either one or no result, where no result means that the matching command is not present
in the new submission.


# ideas

For exact matching, the first idea was to completely match Records, using a longest common subsequence (LCS) algorithm.
This likely does not work since it has O(n^2) complexity (where n would be the number of commands) and since
we can have n > 1000 and have to do this every frame, this will likely not scale as we need it to.

But thinking about it, we don't even need a full record match. We only need to find the match on *one* command.

We also need some metric whether the Records themselves are similar but we can likely solve this in a cheaper
way, without finding an exact match for every command (i.e. compare rough structure and used handles or
something). In the end, even with an exact longest common subsequence algorithm, we might have multiple
matching records in a new frame. For instance, there may be multiple submissions starting the same renderpass
on the same framebuffer, just drawing 4 vertices. Each time with a different descriptorSet/pushConstants/vertexBuffer
but we can determine that with cheaper measures than doing a full match.

So
1. how do match commands, i.e. how do we find the matching command in a new submission (or are sure there is no matching one)?
2. how to find the exact match of records when we have found a matching command in multiple records?
   this is mainly relevant in per-present mode.

Also, do we want to update the selected command description each time we find a new match? We need to allow
matches to have some differences from the originally selected command (to account for newly created descriptors
or uniform buffers; different backbuffer framebuffers etc) so it might be a good idea to update this over time.
But, on the other hand, maybe we should not do it so we can always return to a better match of the originally
selected command (at the risk of suddenly not finding any match at all anymore but I guess this shouldn't
happen when we properly match in the first place). 

Maybe design our match function and command description in a way that for matches it is always 100% equal?
But when we could not include any descriptor data at all, nor framebuffers. Which *might* be what we want,
not sure.

# Matching commands

What exactly must be the same so we consider it a match?
- the position in the command buffer. I.e. if it's under a different label hierachy, cmdExectueCommands, 
  or in a renderpass with a different structure (i.e. different description, pretty much everything is relevant), 
  it can't be a match. Or in a different subpass.
- for draw/dispatch commands: some of the bound state. As observed above, we can't require all state to match.
    - pipeline: must be the same (therefore also the pipeline layout, this is important)
    - descriptors: hard, see below
    - vertex/index buffers: no, must not be the same. But we can take this into consideration.
    - push constants: no, must not be the same. But we can take this into consideration.
- for transfer commands
    - source/destination handles: hm, guess there are cases of "matching" commands where this isn't the same.
      but that's rare.
    - copy regions: we can't require this to be the same, e.g. for uploads the source buffer offset might
      change all the time.
    I guess we should require that at least one of source/dst handles are the same. Also require that
    either src or dst region match (maybe on the same side on which the matching handle is?)
    transfer command matching is hard.
- what about sync or bind commands, do we even need to match them? They are mostly defined by their position though.

harder considerations:
- for draw/dispatch commands: the invoking parameters. A draw with a different number of vertices or dispatch with 
  a different work group count sounds different. But if you e.g. invoke a compute shader for each animation
  on screen at the moment, this may vary per frame. We should likely consider this but not strictly.
    - consider a case where we have many draw commands (for every object in the scene). The order may change
      in every frame for various reasons but the vertex/index count will stay the same in general.
      Yes, the LOD might change but in that case it's ok we don't consider it the same draw call anymore.
      There may also be cases where we don't have much other state to work with (e.g. preZ pass, just the geometry).
    - another difficulty: indirect draw commands. We only know the buffer values *after* the submission is finished.
      Not idea how to properly handle this case. We'd have to analyze where the buffer values come from.
      In case they are already available at submission time (i.e. previously written from host or device), we can
      "just read it" and decide based on it. In case they are dynamically created we could still do stuff with
      a compute shader that does the actual matching of the candidates and then maybe use some conditional rendering
      trickery. But that's terrible, I know.
        Ok, wait I just had a crazy idea on how to match indirect commands. We gather the candidates (somehow) in a
        buffer and insert a compute shader doing the matching. Then we rewrite this following sequence of indirect 
        draw commands so that we capture exactly the one we selected via the compute shader. 
        Not easy but it should work roughly like this. Likely not worth it though.
      The result here is porbably simply this: when using individual indirect commands, matching will not be good.
      We just match the first candidate (maybe be command position or something). 
      When good matching here is desired, use labels.
      This isn't an argumente against using these parameters for matching when available though, making each
      draw command individually indirect isn't very common anyways I guess (either it's fully gpu-driven or not at all).

- for draw/dispatch commands: descriptor state
    - I guess this is something we want to consider. Bound textures can be a huge indicator of what is actually done.
      But it's hard since *some* of this may change dynamically, e.g. uniform buffers may change every frame.
      Hm, maybe the buffers themselves shouldn't actually change. Just the offsets. So maybe we should consider
      the handles but just ignore buffer offsets.
      But doing this strictly seems like a bad idea nonetheless. Shitty applications might create uniform buffer
      (and possibly even images; I'm not surprised by any amount of application shittyness anymore at this point)
      handles every frame, we don't wanna break on them. Just use this for heuristic purposes.
- Framebuffers of render passes. In general we should consider this, framebuffer handles are not dynamically
  created in general. But if you e.g. consider swapchain framebuffers, this may vary per-frame.
  Maybe just detect the backbuffer framebuffer case and handle it? I.e. any swpachain framebuffer matches with
  any other framebuffer from the same swapchain. 

With these notes, we have to use heuristics. Otherwise we can't take all information into account we want.
But in general we should probably just advise that developers should use tons of labels to improve matching
(if that is needed; in most cases exact matching isn't relevant in the first place).
We should probably introduce a special label prefix that causes label not be shown in the ui but considered
for matching.

Crazy idea: consider callstacks
- two commands coming from a different callstack can't be the same. On the other hand, not all commands coming
  from the same callstack are the same. But we could use it as pre-sorting.
  I can't think of a situation where this can ever produce false negatives. At worst, it doesn't catch anything
  (e.g. if the application has some internal command representation and buffering, in which case we wouldn't
  get the callstack at which a command originates)

---

So much of theses commands depends on position though. Matching sync/bind commands is pretty much impossibly.
Maybe we can implement some LCS algorithm after all, with some clever tricks to cut down the number of
commands. LCS isn't optimally solving our problem anyways, we don't need/want to consider the order of
draw commands this strongly, it's more like a set. 
Well, technically, the order is important only when not using a depth buffer/
pipelines without depth testing/writing.

Roughly same for most transfer commands/dispatch commands really. We are probably able to detect possible
data dependencies at submission time and just parse everything into (unordered) sets of commands.

---

For the rough structure inside a record we probably want to use LCS. But inside
of a render pass we can't rely on it. Same for a block of transfer commands,
and probably also compute commands. There, we mainly want to match by state.

What about matching bind/sync commands?
bind commands: Just match the next draw/dispatch commands and go back from
	that to this bind command. When there is no following draw/dispatch this
	bind is bogus anyways, just match with the last bind.
sync commands: Match the last and next draw/dispatch/transfer commands
	and then try to find a sync between them, if there is none, fail?
	We have some more information here though, matching by state could
	be mildly succesful.

# Matching records

So, how do find a matching record, e.g. when in per-present matching mode.
First idea: LCS matching for CommandRecords.

---

When considering handles for matching records/commands we should think
about handle persistence. Very persistent handles are, for instance,
VkPipeline and (to a lesser degree) VkImage. Short-lived handles
are VkDescriptorSet and maybe (to a lesser degree) VkBuffer. We mainly
want to consider the persistent handles, or at least give them a waaay
higher weight.

## Case study; Global finding

Inspired by RDR2. Consider this case:
An application submits 100 cbs in the pattern (work, barrier, work, barrier, ...),
where work are the real command buffers containing render passes and 
dispatches while 'barrier' is a command buffer just contains a single pipeline
barrier. (such a pattern isn't too unlikely with multi threaded rendering I guess)

Selecting one of the CmdPpipelineBarrier commands will completely break our 
hooking mechanism since we will likely hook *all* of the 'barrier' command buffers.
Even if we then select the correct one in the end via LCS, this causes
horrible performance.

This can only be fixed by already considering the context (beyond command
buffer boundaries) when deciding whether to match or not.
In this case, we need some prefix matching.

Previous frame submissions: A B C D E F
Selected command: a CmdPipelineBarrier in C

This frame, so far: A' B' C' D'

Now we want to check if we should hook C' (let's say e.g. C' and D' came
in a single submission, i.e. we already know D').
We can now do a LCS between the previous frame and what we know so far.
When C' and C are selected (and even have a significant match), we
go for it. 
If they are not selected as match (e.g. C' is matched to E or with nothing at all)
then we don't hook.
If C and C' are matched by have a very low match maybe also skip the hook? not sure.
Maybe only if we already had a better candidate this frame?

---

# The accumulated frame

Accumulate via command groups: [...]
