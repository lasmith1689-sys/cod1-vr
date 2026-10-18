"""Measured Thompson level-hold calibration; no controller convention guesses.

Row basis: base=C*grip. An anatomical vector b becomes b*M*C*grip
after the operator trim M. For a desired grip-frame rotation R (column
vectors), M=C*R.T*C.T. Positions do not enter this calculation.
"""
from pathlib import Path
import re, json
import numpy as np

root=Path(__file__).resolve().parents[1]
text=(root/'src/vr_palm_calibration.inc').read_text()
rows=re.findall(r'"viewmodel_hands_new4", VRHP_\w+,\s*\{([^}]+)\}',text)
cal=[np.array([float(x.strip().rstrip('f')) for x in a.split(',')]).reshape(3,3) for a in rows]
T=np.array([[0,0,-1],[-1,0,0],[0,1,0.]])
bore=T@np.array([.0724,-.9871,.1425]); bore/=np.linalg.norm(bore)
desired=T@np.array([.0386,-.9655,-.2576]); desired/=np.linalg.norm(desired)
v=np.cross(bore,desired); c=np.dot(bore,desired)
K=np.array([[0,-v[2],v[1]],[v[2],0,-v[0]],[-v[1],v[0],0]])
rot=np.eye(3)+K+K@K/(1+c)
mirror=np.diag([1,-1,1])
result={}
for side,C,R in zip(('right','left'),cal,(rot,mirror@rot@mirror)):
    M=C@R.T@C.T
    p,y,r=np.rad2deg([np.arcsin(-M[0,2]),np.arctan2(M[0,1],M[0,0]),np.arctan2(M[1,2],M[2,2])])
    result[side]={'pitch':round(float(p),2),'yaw':round(float(y),2),'roll':round(float(r),2),'matrix':M.tolist()}
result['angle_degrees']=float(np.rad2deg(np.arccos(c)))
print(json.dumps(result,indent=2))
