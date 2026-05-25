# 0.2.0
- Updated, after refactoring loraham_daemon to 109a. *This breaks comatibility to loraham_daemon < 109*
- Use framed DATA sockets instead of raw Sockets
- TX Queue and delayed sending on TX=1 or CAD=1
- Use persistent CONF socket

# 0.1.0
- Initial Version, developed for loraham_daemon 108
- Queue TX packets and use CONF status events.
- Track CONF event transitions for TX queue tests.
- Extract bridge CONF event helpers.
- Extract bridge TX queue helpers.
