"""Socket generation: vision finds the parts, geometry measures the sockets.

``views``     run ptv_glb_views and load renders + triangle-id buffers
``vision``    ask Gemini for part masks in every view
``fusion``    turn per-view masks into triangle regions on the mesh
``solvers``   per socket kind: regions -> exact local transforms
``verify``    overlay renders + a second Gemini pass that checks the result
``fallback``  geometry-only sockets when no vision model is available
"""
