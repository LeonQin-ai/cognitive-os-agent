"""Exercise Windows cancellation errors at every response-write boundary."""
import unittest
from fixture_http_response import respond_json

class Handler:
    def __init__(self, fail=None, error=None):
        self.fail,self.error=fail,error;self.wfile=self;self.sent=[]
    def step(self, name, value=None):
        if name==self.fail:raise self.error
        self.sent.append((name,value))
    def send_response(self,value):self.step('status',value)
    def send_header(self,key,value):self.step('header',(key,value))
    def end_headers(self):self.step('headers')
    def write(self,body):self.step('body',body)

class ResponseTests(unittest.TestCase):
    def test_live_response_is_complete(self):
        handler=Handler();self.assertTrue(respond_json(handler,b'{}',503))
        self.assertEqual(handler.sent[0],('status',503))
        self.assertIn(('header',('Content-Length','2')),handler.sent)
        self.assertEqual(handler.sent[-1],('body',b'{}'))
    def test_cancelled_peer_during_headers_and_body(self):
        for stage in ('status','header','headers','body'):
            for error in (BrokenPipeError,ConnectionResetError,ConnectionAbortedError):
                with self.subTest(stage=stage,error=error):
                    self.assertFalse(respond_json(Handler(stage,error(10053,'cancelled fixture peer')),b'{}'))
    def test_unexpected_provider_errors_remain_visible(self):
        with self.assertRaises(PermissionError):
            respond_json(Handler('headers',PermissionError('real fixture failure')),b'{}')
if __name__=='__main__':unittest.main()
