import sys
p = sys.argv[1]
s = open(p, encoding='utf-8').read()
helper = '''
// SDNX diagnostic (Sleeping Dogs allocator pool corruption, 2026-11-13): a critical section acquired while another
// thread still owns it, or released by a thread that does not own it, is logged (first 32 times).
static void SdnxCritSecReport(const char* what, uint32_t cs_address, uint32_t owner, uint32_t self, int32_t lock_count) {
  static std::atomic<int> reported{0};
  if (reported.fetch_add(1, std::memory_order_relaxed) < 32) {
    REXKRNL_ERROR("[critsec] {} cs={:#010x} owner={:#010x} self={:#010x} lock_count={}", what, cs_address, owner, self,
                  lock_count);
  }
}

void RtlEnterCriticalSection_entry('''
assert s.count('\nvoid RtlEnterCriticalSection_entry(') == 1
s = s.replace('\nvoid RtlEnterCriticalSection_entry(', helper, 1)
old_spin = '''      // Acquired.
      cs->owning_thread = cur_thread;'''
assert s.count(old_spin) == 1
s = s.replace(old_spin, '''      // Acquired.
      if (uint32_t owner = cs->owning_thread; owner != 0) {
        SdnxCritSecReport("acquired (spin) while owned", cs.guest_address(), owner, cur_thread, cs->lock_count);
      }
      cs->owning_thread = cur_thread;''')
old_wait = '''  assert_true(cs->owning_thread == 0);
  cs->owning_thread = cur_thread;'''
assert s.count(old_wait) == 1
s = s.replace(old_wait, '''  if (uint32_t owner = cs->owning_thread; owner != 0) {
    SdnxCritSecReport("acquired while owned", cs.guest_address(), owner, cur_thread, cs->lock_count);
  }
  cs->owning_thread = cur_thread;''')
old_leave = '''  assert_true(cs->owning_thread == XThread::GetCurrentThread()->guest_object());'''
assert s.count(old_leave) == 1
s = s.replace(old_leave, '''  if (uint32_t self = XThread::GetCurrentThread()->guest_object(), owner = cs->owning_thread; owner != self) {
    SdnxCritSecReport("released by a non-owner", cs.guest_address(), owner, self, cs->lock_count);
  }''')
if '#include <atomic>' not in s:
    s = s.replace('#include <', '#include <atomic>\n#include <', 1)
open(p, 'w', encoding='utf-8', newline='\n').write(s)
print('ok')
