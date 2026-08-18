// Synthetic rail ids, in one leaf module.
//
// These are shared by `lifecycle` (a deliberately dependency-light state module)
// and `settings/rail` (which pulls in the mod/view record types). Neither should
// have to import the other just to agree on an id string, and two independent
// definitions of the same literal are a silent-drift hazard — hence this leaf.

/**
 * The Home launcher's rail id. ':' is not legal in the filesystem-backed mod-id
 * namespace, so a real mod cannot shadow Home.
 */
export const HOME_ID = ':home';
