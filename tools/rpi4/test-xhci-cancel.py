#!/usr/bin/env python3
"""Run the production cancellation routine through a deterministic enqueue race.

StopEndpoint is an explicit scheduling point: the endpoint lock is dropped,
so a report callback or reader can submit another transfer at that instant.
The list/refcount fixtures observe lost Transfers and double completions. The
production link admission code is extracted before its hardware-ring writes.
Use --baseline REV to confirm that a pre-fix revision leaks a Pipe ref.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--baseline', metavar='REV', help='test this Git revision')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
path = 'src/add-ons/kernel/busses/usb/xhci.cpp'
source = (subprocess.check_output(['git', 'show', args.baseline + ':' + path],
          cwd=root).decode() if args.baseline else (root / path).read_text())

def function(name):
    start = source.index('\n' + name + '(') + 1
    start = source.rfind('\n', 0, start - 1) + 1
    body = source.index('{', start)
    depth, end = 1, body + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'

cancel = function('XHCI::CancelQueuedTransfers')
link = function('XHCI::_LinkDescriptorForPipe').split('\n\t// "used" refers')[0]
link += '\n endpoint->td_list.Add(descriptor); ++endpoint->used; return B_OK;\n}\n'
fixture = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>
using uint32 = uint32_t;
using int32 = int32_t;
using status_t = int;
constexpr int B_OK=0, B_NO_INIT=-1, B_DEV_STALLED=-2, B_CANCELED=-3, B_ERROR=-4;
constexpr int XHCI_MAX_TRANSFERS=16, XHCI_ENDPOINT_RING_SIZE=32;
#define TRACE(...) ((void)0)
#define TRACE_ERROR(...) ((void)0)
#define TRACE_ALWAYS(...) ((void)0)
struct mutex { bool held=false; };
int mutex_trylock(mutex* m) { assert(!m->held); m->held=true; return B_OK; }
struct MutexLocker {
 mutex* m; bool held;
 MutexLocker(mutex& v): m(&v), held(false) { Lock(); }
 MutexLocker(mutex* v,bool h): m(v), held(h) { if(!h) Lock(); }
 ~MutexLocker() { if(held) Unlock(); }
 void Lock() { assert(!held && !m->held); held=m->held=true; }
 void Unlock() { assert(held && m->held); held=m->held=false; }
};
template<class T> struct DoublyLinkedList {
 std::vector<T*> items;
 T* Head() { return items.empty()?nullptr:items.front(); }
 T* GetNext(T* p) { auto i=std::find(items.begin(),items.end(),p); assert(i!=items.end()); return ++i==items.end()?nullptr:*i; }
 void Add(T* p) { items.push_back(p); }
 T* RemoveHead() { T* p=Head(); if(p) items.erase(items.begin()); return p; }
 void TakeFrom(DoublyLinkedList* v) { assert(items.empty()); items.swap(v->items); }
};
struct xhci_endpoint;
struct Pipe {
 xhci_endpoint* endpoint; int refs=1; uint32 id=1;
 void* ControllerCookie() { return endpoint; }
 int EndpointAddress() { return 1; }
 uint32 USBID() { return id; }
};
static int live=0, callbacks=0, freedWithTransfer=0;
struct Transfer {
 Pipe* pipe; bool finished=false;
 Transfer(Pipe* p):pipe(p) { ++live; ++p->refs; }
 ~Transfer() { --live; --pipe->refs; }
 Pipe* TransferPipe() { return pipe; }
 void Finished(int status,int) { assert(!finished && status==B_CANCELED); finished=true; ++callbacks; }
};
struct xhci_trb { uint64_t data[2]; };
struct xhci_td { Transfer* transfer; };
struct xhci_device { int slot=1; };
struct xhci_endpoint {
 mutex lock;
 int used=0, next=0, id=2;
 uint32 cancel_count=0;
 xhci_device* device;
 xhci_trb* trbs;
 uint64_t trb_addr=0;
 DoublyLinkedList<xhci_td> td_list;
};
struct XHCI {
 std::function<void()> onStop;
 int stopStatus=B_OK;
 status_t CancelQueuedTransfers(Pipe*,bool);
 status_t _LinkDescriptorForPipe(xhci_td*,xhci_endpoint*);
 status_t StopEndpoint(bool,xhci_endpoint* e) {
  assert(!e->lock.held);
  auto callback=std::move(onStop); onStop=nullptr;
  if(callback) callback();
  return stopStatus;
 }
 status_t ResetEndpoint(bool,xhci_endpoint* e) { assert(!e->lock.held); return B_OK; }
 void SetTRDequeue(uint64_t,int,int,int) {}
 void FreeDescriptor(xhci_td* td) { if(td->transfer) ++freedWithTransfer; delete td; }
 status_t Queue(Pipe* pipe) {
  auto* t=new Transfer(pipe); auto* td=new xhci_td{t};
  int status=_LinkDescriptorForPipe(td,pipe->endpoint);
  if(status!=B_OK) { td->transfer=nullptr; FreeDescriptor(td); delete t; }
  return status;
 }
};
'''
tests = r'''
int main() {
 for(int variant=0;variant<5;variant++) {
  XHCI host; xhci_device device; xhci_trb trbs[XHCI_ENDPOINT_RING_SIZE]{};
  xhci_endpoint ep; ep.device=&device; ep.trbs=trbs; Pipe pipe{&ep};
  assert(host.Queue(&pipe)==B_OK);
  int admission=B_OK;
  host.onStop=[&] {
   if(variant==1) {
    host.onStop=[&] { admission=host.Queue(&pipe); };
    host.CancelQueuedTransfers(&pipe,false);
    // The inner cancellation must not reopen admission while the outer
    // operation still owns its snapshot and is about to clear the ring.
    admission=host.Queue(&pipe);
   } else admission=host.Queue(&pipe);
  };
  if(variant==2) host.stopStatus=B_ERROR;
  if(variant==3) host.stopStatus=B_DEV_STALLED;
  bool force=variant==4;
  int oldCallbacks=callbacks;
  host.CancelQueuedTransfers(&pipe,force);
  if(freedWithTransfer || pipe.refs!=1 || live!=0) {
   fprintf(stderr,"LEAK: refs=%d live=%d descriptors_freed_with_transfer=%d\n",pipe.refs,live,freedWithTransfer);
   return 1;
  }
  assert(admission==B_CANCELED && ep.cancel_count==0);
  assert(callbacks-oldCallbacks==(force?0:1));
  // A failed stop keeps empty descriptors until hardware can retire them.
  host.stopStatus=B_OK; host.CancelQueuedTransfers(&pipe,false);
  assert(ep.td_list.Head()==nullptr);
  assert(host.Queue(&pipe)==B_OK);
  host.CancelQueuedTransfers(&pipe,false);
  assert(pipe.refs==1 && live==0);
  pipe.id=UINT32_MAX;
  assert(host.Queue(&pipe)==B_CANCELED);
  assert(pipe.refs==1 && live==0);
 }
 puts("XHCI cancellation/enqueue, overlapping cancellation, failed stop, stall, force, retired pipe: passed");
}
'''
with tempfile.TemporaryDirectory(prefix='xhci-cancel-', dir='/mnt/HaikuWork/tmp') as work:
    out=Path(work)
    (out/'test.cpp').write_text(fixture + cancel + link + tests)
    subprocess.run(['c++','-std=c++17','-g','-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer',str(out/'test.cpp'),'-o',str(out/'test')],check=True)
    subprocess.run([str(out/'test')],check=True)
