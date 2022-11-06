If one event has 100B and we have ~10 events per frame on avg 
-> ~600 events per second 
-> 6KB per second 
-> 360KB per minute 
-> 21MB per hour.
So having a high lifetime for the events is totally fine.
If we count all descriptor sets and stuff, we have ofc way more than 10 events
per frame.
Either only track them when the tab is open or give them a way shorter
lifetime.
A total lifetime of a couple of minutes should be totally fine.
We just want the eventlog to not be super-short-lived so it's possible
to see very exactly what happened during startup, in in which order.

When the tab is open, maybe it should take ownership of the LoggedEvent
vector somehow? Make sure it does not scroll away...
maybe add manual clear/freeze buttons?
