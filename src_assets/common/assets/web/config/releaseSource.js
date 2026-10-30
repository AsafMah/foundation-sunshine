/* global __FOUNDATION_RELEASE_REPOSITORY__ */
// Release repository the Web UI checks for Sunshine updates.
// Official builds keep the historical repository (GitHub redirects it to
// AlkaidLab/foundation-sunshine). Fork builds set FOUNDATION_RELEASE_REPOSITORY
// at build time, as for the control panel updater, so the update notice only
// offers the fork's own releases.
const OFFICIAL_RELEASE_REPOSITORY = 'qiin2333/Sunshine'
const REPOSITORY_PATTERN = /^[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+$/

export const resolveReleaseRepository = (configured) => {
  const value = typeof configured === 'string' ? configured.trim() : ''
  return REPOSITORY_PATTERN.test(value) ? value : OFFICIAL_RELEASE_REPOSITORY
}

export const RELEASE_REPOSITORY = resolveReleaseRepository(
  typeof __FOUNDATION_RELEASE_REPOSITORY__ === 'undefined' ? '' : __FOUNDATION_RELEASE_REPOSITORY__,
)
