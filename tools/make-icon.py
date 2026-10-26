#!/usr/bin/env python3
"""Original Haiku film projector and play symbol. MIT licence."""
import math
from icon_art import Drawing, INK, plane

d = Drawing()
p = plane(10, 26)
d.shadow([(10,46),(37,59.5),(62,59.5),(35,46)])
# Projector housing: lit top, front with play control, shaded right side.
d.shape([(10,26),(22,14),(49,27.5),(37,39.5)], d.gradient('e5edf1','a4c1d0'), INK)
d.shape([(37,39.5),(49,27.5),(49,47.5),(37,59.5)], d.gradient('52788d','2e4f64'), INK)
d.shape(p([(0,0),(27,0),(27,20),(0,20)]), d.gradient('b6d9e1','5c91ae'), INK)
d.shape(p([(6,4),(6,16),(19,10)]), d.gradient('fff5d5','dcc99a'), '3d677b', 1.5)
# Paired silver reels are the broad movie metaphor, without glossy badges.
for x,y,r in [(18,15,10),(40,21,10)]:
    d.ellipse(x,y,r,r,d.gradient('fffde9','b5bcb1',(x-8,y-8),(x+7,y+9)),INK)
    for angle in (0,120,240):
        a=math.radians(angle)
        d.ellipse(x+4.8*math.cos(a),y+4.8*math.sin(a),2.2,2.2,'5e757a')
    d.ellipse(x,y,1.5,1.5,'6e8589')
# The lens projects from the dark side; its curved glass gets one restrained highlight.
d.shape([(45,33),(54,35),(56,47),(47,45)], d.gradient('839da2','415d69'), INK, 1.5)
d.ellipse(54,41,5,7,d.gradient('a6cbd0','416c7e'),INK,1.8)
d.ellipse(54,41,2.7,4.5,d.gradient('bce4e3','447d98'), '284d60',1)
d.line([(53,38),(53,40)],'e2f8eb',1.2)
d.save(__file__, 'resources/branding/airtime-icon.hvif')
