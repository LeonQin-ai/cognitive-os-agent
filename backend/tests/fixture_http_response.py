"""Finite JSON fixture replies; cancelled clients may leave before headers or body."""
def respond_json(handler, body, status=200):
    try:
        handler.send_response(status)
        handler.send_header('Content-Type', 'application/json')
        handler.send_header('Content-Length', str(len(body)))
        handler.end_headers()
        handler.wfile.write(body)
    except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
        # Windows can abort during end_headers(), not just the final body write.
        # This is an expected peer disconnect, not a provider assertion failure.
        return False
    return True
