import assert from 'node:assert/strict'
import test from 'node:test'

import SunshineVersion from '../sunshine_version.js'
import { RELEASE_REPOSITORY, resolveReleaseRepository } from '../config/releaseSource.js'

const newer = (candidate, installed) => new SunshineVersion(null, candidate).isGreater(installed)

test('official builds keep the historical release repository', () => {
  assert.equal(RELEASE_REPOSITORY, 'qiin2333/Sunshine')
  assert.equal(resolveReleaseRepository(''), 'qiin2333/Sunshine')
  assert.equal(resolveReleaseRepository('not a repo'), 'qiin2333/Sunshine')
  assert.equal(resolveReleaseRepository(' AsafMah/foundation-sunshine '), 'AsafMah/foundation-sunshine')
})

test('fork release versions order between upstream releases', () => {
  const fork = 'v2026.1029.95137.6.杂鱼'
  assert.equal(newer(fork, 'v2026.1029.95137.杂鱼'), true)
  assert.equal(newer(fork, 'v2026.1029.95137.5.杂鱼'), true)
  assert.equal(newer(fork, 'v2026.1027.141040.5.杂鱼'), true)
  assert.equal(newer(fork, '0.0.0.5379d6f6f10284f1e5f5b7c65a99a24618b49a46'), true)
  assert.equal(newer(fork, fork), false)
  assert.equal(newer('v2026.1029.95137.杂鱼', fork), false)
  assert.equal(newer('v2026.1030.10000.杂鱼', fork), true)
})

test('official versions still compare by their numeric release parts', () => {
  assert.equal(newer('v2026.1029.95137.杂鱼', 'v2026.1029.95137.杂鱼'), false)
  assert.equal(newer('v2026.1029.95137.杂鱼', 'v2026.1027.141040.杂鱼'), true)
  assert.equal(newer('v2026.1027.141040.杂鱼', 'v2026.1029.95137.杂鱼'), false)
})
