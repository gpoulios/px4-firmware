# Ack a previously sent ulog_stream_acked message that had
# the NEED_ACK flag set

uint64 timestamp		# time since system start (microseconds)
int32 ACK_TIMEOUT = 50		# timeout waiting for an ack until we retry to send the message [ms]
int32 ACK_MAX_TRIES = 50	# maximum amount of tries to (re-)send a message, each time waiting ACK_TIMEOUT ms

uint16 msg_sequence
