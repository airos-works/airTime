#!/usr/bin/env python3
"""Original Haiku movie clapperboard and play symbol, joined as one object. MIT."""
from icon_art import Drawing, INK, plane

d = Drawing()
p = plane(8, 8, 0.25)
project = lambda point: p([point])[0]
back = lambda point: (point[0]+11,point[1]+5+0.25*point[0])
d.shadow([(9,48),(49,59),(62,59),(34,48)])
# A closed clapperboard: one coherent silhouette, a thin side and a blue slate.
d.rounded_rect(0,0,42,40,2.5,d.gradient('64899b','2f4d61'),INK,2,back)
d.rounded_rect(0,0,42,40,2.5,d.gradient('85b6cb','3c7597'),INK,2,project)
# The clapper is closed against the slate, with broad, readable diagonal stripes.
d.rounded_rect(0,0,42,10,2.5,d.gradient('4b5b64','233846'),INK,1.5,project)
for points in [[(1,2),(6,2),(11,8),(5,8)],[(12,2),(18,2),(23,8),(17,8)],
               [(24,2),(30,2),(35,8),(29,8)],[(36,2),(40,2),(40,8)]]:
    d.shape(p(points),d.gradient('fff8df','d9dfcf',(10,9),(49,22)))
d.line(p([(1,10),(41,10)]),'1d394a',1.4)
# The play symbol identifies a movie player rather than a video editor.
d.shape(p([(15,16),(15,32),(29,24)]),d.gradient('fff7cf','e7c576',(23,28),(35,48)))
d.save(__file__, 'resources/branding/airtime-icon.hvif')
