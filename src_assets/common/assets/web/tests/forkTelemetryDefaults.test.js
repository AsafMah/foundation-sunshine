import test from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'

const read = (p) => fs.readFileSync(new URL(p, import.meta.url), 'utf8')

// Fork (AsafMah/foundation-sunshine): telemetry is off until the person turns it on (src/telemetry/fork_defaults.h). The first
// screen's one-time notice says data is sent, so the guide asks the host first and skips that notice while both choices are off.
test('fork: the first-run guide does not say data is sent while both telemetry choices are off', () => {
  const host = read('../components/GuideHost.vue')
  const mounted = host.slice(host.indexOf('onMounted(async () => {'))
  const refresh = mounted.indexOf('await telemetry.refresh()')
  const progress = mounted.indexOf('hostProgress.value =')
  assert.ok(refresh >= 0 && progress > refresh, 'the host answers about telemetry before the guide (and its notice) is drawn')
  assert.match(mounted, /if \(telemetry\.consent\.value === 'off' && telemetry\.errors\.value === 'off'\) telemetry\.markErrorsNoticeSeen\(\)/)
})