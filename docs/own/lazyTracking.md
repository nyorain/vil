# Lazy tracking optimization

Many games
- never reuse command buffers
- record all command buffers in every frame
- update most descriptors in every frame

E.g. let's consider a game that does that and for descriptors,
it has some VkDescriptorPools that are permanent while most are transient
(i.e. descriptors never re-used).

When the overlay is not open, we don't need to track track CommandRecords
(saving a lot of recording overhead) and don't even need to track
updating of descriptor sets from the transient pools (saving A LOT
of overhead there as well). I guess we could even get around tracking
the existence of these transient descriptors? sounds harder though.

Only once the overlay is opened, we start to track all that.

---

Let's skip the descriptor part for now. We have to unwrap a lot of
stuff in there anyways. 
Hm that's actually an argument against unwrapping: faster fast-path forwarding. 
Damn.

Also, I'm not so sure we gain a lot from doing this for command recording.
We need to unwrap (and therefore copy a lot of structs) anyways.
So shouldn't be such a huge optimization, there will still be overhead
afterwards.
Let's not bother for now.
