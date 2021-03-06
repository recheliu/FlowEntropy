#include "liblog.h"

#include <GL/glew.h>

#include <GL/glut.h>

#define _USE_MATH_DEFINES
#include <math.h>

#include "cuda_macro.h"	

#include "Streamline.h"

#define RENDER_STREAMLINE_AS_LINES		1

#define RENDER_STREAMLINE_AS_TUBES		0

#define	SORT_ON_CUDA		0
#ifdef	__DEVICE_EMULATION__
	#define SORT_ON_CUDA	0
#endif
int 
ISortSlab(const void *p0, const void *p1)
{
	const int2 *pi2Src0 = (const int2*)p0; 
	const int2 *pi2Src1 = (const int2*)p1; 

	if(pi2Src0->x < pi2Src1->x )
		return -1;
	if(pi2Src0->x > pi2Src1->x )
		return +1;
	if(pi2Src0->y < pi2Src1->y )
		return -1;
	if(pi2Src0->y > pi2Src1->y )
		return +1;
	return 0;
}

void
CStreamline::_SortSlab(
	int iNrOfSlabs,
	double pdModelViewMatrix[],
	double pdProjectionMatrix[],
	int piViewport[]
)
{
	if( !pi2BaseLengths.BIsAllocated() || iNrOfSlabs != pi2BaseLengths.USize() )
		pi2BaseLengths.alloc(iNrOfSlabs);
	memset(&pi2BaseLengths[0], 0, sizeof(pi2BaseLengths[0]) * pi2BaseLengths.USize());

								// find the bounding box in eye space
	static double pdIdentityMatrix[16] = {
		1.0, 0.0, 0.0, 0.0,
		0.0, 1.0, 0.0, 0.0,
		0.0, 0.0, 1.0, 0.0,
		0.0, 0.0, 0.0, 1.0,
	};
	static GLdouble pdCornerCoord[][3] = {
		{-1.0, -1.0, -1.0},
		{-1.0, -1.0,  1.0},
		{-1.0,  1.0, -1.0},
		{-1.0,  1.0,  1.0},
		{ 1.0, -1.0, -1.0},
		{ 1.0, -1.0,  1.0},
		{ 1.0,  1.0, -1.0},
		{ 1.0,  1.0,  1.0},
	};

	double dMinX, dMinY, dMinZ;
	double dMaxX, dMaxY, dMaxZ;

	dMinX = dMinY = dMinZ = HUGE_VAL;
	dMaxX = dMaxY = dMaxZ = -HUGE_VAL;
	for(int i=0; i<sizeof(pdCornerCoord)/sizeof(pdCornerCoord[0]); i++) 
	{
		double dX_win, dY_win, dZ_win;
		double dX_eye, dY_eye, dZ_eye;
		gluProject(
			pdCornerCoord[i][0], pdCornerCoord[i][1], pdCornerCoord[i][2],
			pdModelViewMatrix, pdProjectionMatrix, piViewport,
			&dX_win, &dY_win, &dZ_win);

		gluUnProject(
			dX_win, dY_win, dZ_win,
			pdIdentityMatrix, pdProjectionMatrix, piViewport,
			&dX_eye, &dY_eye, &dZ_eye);

		dMinX = min(dMinX, dX_eye);
		dMaxX = max(dMaxX, dX_eye);
		dMinY = min(dMinY, dY_eye);
		dMaxY = max(dMaxY, dY_eye);
		dMinZ = min(dMinZ, dZ_eye);
		dMaxZ = max(dMaxZ, dZ_eye);
	}

	// sort the centroids according to their depth
	#if	!SORT_ON_CUDA

	if( false == piNrOfLinesPerSlab.BIsAllocated() || iNrOfSlabs != piNrOfLinesPerSlab.USize() )
		piNrOfLinesPerSlab.alloc(iNrOfSlabs);
	else
		memset(&piNrOfLinesPerSlab[0], 0, sizeof(piNrOfLinesPerSlab[0]) * piNrOfLinesPerSlab.USize());

	if( false == piLineOffsetPerSlab.BIsAllocated() || iNrOfSlabs != piLineOffsetPerSlab.USize() )
		piLineOffsetPerSlab.alloc(iNrOfSlabs);
	else
		memset(&piLineOffsetPerSlab[0], 0, sizeof(piLineOffsetPerSlab[0]) * piLineOffsetPerSlab.USize());

	for(int l = 0; l < int(uNrOfLines); l++)
	{
		double dDepth = 
			pdModelViewMatrix[2]	* pfLineCentroids[l * 3 + 0] + 
			pdModelViewMatrix[6]	* pfLineCentroids[l * 3 + 1] + 
			pdModelViewMatrix[10]	* pfLineCentroids[l * 3 + 2] + 
			pdModelViewMatrix[14]; 

		int iSlab = int(double(iNrOfSlabs) *  (dDepth - dMinZ) / (dMaxZ - dMinZ));
		iSlab = min(max(iSlab, 0), iNrOfSlabs - 1);

		pi2SlabTemp[l].x = iSlab; 
		pi2SlabTemp[l].y = l; 
		piNrOfLinesPerSlab[iSlab]++;
	}

	// sort the indices by the slab indices
	for(int s = 1; s < iNrOfSlabs; s++)
		piLineOffsetPerSlab[s] = piLineOffsetPerSlab[s-1] + piNrOfLinesPerSlab[s-1];

	for(int l = 0; l < int(uNrOfLines); l++)
	{
		int iSlab = pi2SlabTemp[l].x;
		pi2Slabs[piLineOffsetPerSlab[iSlab]++] = pi2SlabTemp[l];
	}

	#else	// #if	!SORT_ON_CUDA
	_ComputeDepth_cuda
	(
		iNrOfSlabs,

		dMinZ,
		dMaxZ,
		pdModelViewMatrix,
		pdProjectionMatrix,

		int(uNrOfLines),
		&pi2Slabs[0]
	);
	#endif	// #if	!SORT_ON_CUDA

	// reorganize the vertices indices
	for(int iActualNrOfLines = 0, l = 0; l < int(uNrOfLines); l++)
	{
		int iS = pi2Slabs[l].x;	// slab index
		int iL = pi2Slabs[l].y;	// line index
		int iStreamline = puLineSegmentIndicesToStreamlines[iL]; 

		// only consider the first uMaxNrOfStreamlines streamlines
		if( iStreamline >= int(uMaxNrOfStreamlines) || iStreamline < int(uMinNrOfStreamlines) )
			continue;

		if( 0 != iStreamline % int(uSamplingRate) )
			continue;

		pi2BaseLengths[iS].y++; // increment the counter for the corresponding slab
		for(int i = 0; i < 2; i++)
			pu2SortedLineSegmentIndicesToVertices[2 * iActualNrOfLines + i] = pu2LineSegmentIndicesToVertices[2 * iL + i];
		iActualNrOfLines++;
	}
	for(int s = 1; s < iNrOfSlabs; s++)
		pi2BaseLengths[s].x += pi2BaseLengths[s-1].x + pi2BaseLengths[s-1].y;
}


void 
CStreamline::_Read(float fScaleX, float fScaleY, float fScaleZ, char *szStreamlineFilename, int iMaxNrOfLoadedStreamlines)
{
	FILE *fpStreamline;
	fpStreamline = fopen(szStreamlineFilename, "rb");
	assert(fpStreamline);

	unsigned int uNrOfStreamlines;
	fread(&uNrOfStreamlines, sizeof(uNrOfStreamlines), 1, fpStreamline);

	int iActualNrOfStreamlines = uNrOfStreamlines;
	if( iMaxNrOfLoadedStreamlines >= 0 )
		uNrOfStreamlines = min(int(uNrOfStreamlines), iMaxNrOfLoadedStreamlines);

	uNrOfLines = 0;

	for(unsigned int uS = 0; uS < uNrOfStreamlines; uS++)
	{
		unsigned int uV;
		fread(&uV, sizeof(uV), 1, fpStreamline);
		uNrOfVertices += uV;
		uNrOfLines += uV - 1;
		vuNrOfVertices.push_back(uV);
	}

	fseek(fpStreamline, sizeof(unsigned int) * (iActualNrOfStreamlines - uNrOfStreamlines), SEEK_CUR);

	LOG_VAR(uNrOfLines);
	LOG_VAR(uNrOfVertices);

	uMinNrOfStreamlines = 0;
	uMaxNrOfStreamlines = uNrOfStreamlines;

	pfCoords.alloc(3 * uNrOfVertices);
	pfTangent.alloc(3 * uNrOfVertices);

	pi4VertexIndicesInStreamline.alloc(uNrOfVertices);

	pu2LineSegmentIndicesToVertices.alloc(2 * uNrOfLines);

	puLineSegmentIndicesToStreamlines.alloc(uNrOfLines);

	pu2SortedLineSegmentIndicesToVertices.alloc(2 * uNrOfLines);
	pfLineCentroids.alloc(3 * uNrOfLines);
	pi2Slabs.alloc(uNrOfLines);
	pi2SlabTemp.alloc(uNrOfLines);	

	unsigned int uCoordIndex = 0;
	unsigned int uLineIndex = 0;
	unsigned int uStreamline = 0;
	for(vector<unsigned int>::iterator 
			ivuNrOfVertices = vuNrOfVertices.begin();
		ivuNrOfVertices != vuNrOfVertices.end();
		ivuNrOfVertices ++, uStreamline++)
	{
		unsigned int uNrOfVertices = *ivuNrOfVertices;

		fread(
			&pfCoords[uCoordIndex * 3], 
			sizeof(pfCoords[0]), 
			uNrOfVertices * 3, 
			fpStreamline);

		for(unsigned int uV = 0; uV < uNrOfVertices; uV++)
		{
			pi4VertexIndicesInStreamline[uCoordIndex + uV].x = int(uV);
			pi4VertexIndicesInStreamline[uCoordIndex + uV].y = int(uStreamline);
			pi4VertexIndicesInStreamline[uCoordIndex + uV].z = 0;
			pi4VertexIndicesInStreamline[uCoordIndex + uV].w = 0;
		}

		for(unsigned int uV = 0; uV < uNrOfVertices-1; uV++, uLineIndex++)
		{
			pu2LineSegmentIndicesToVertices[uLineIndex*2] = uCoordIndex + uV;
			pu2LineSegmentIndicesToVertices[uLineIndex*2+1] = uCoordIndex + uV + 1;

			puLineSegmentIndicesToStreamlines[uLineIndex] = uStreamline;

			// compute tangents
			if( uV > 0 )
			{
				int iVertexIndex = uCoordIndex + uV;

				float fLength = 0.0f;
				for(int i = 0; i < 3; i++)
				{
					float fDiff = pfCoords[3 * (iVertexIndex + 1) + i] - pfCoords[3 * (iVertexIndex - 1) + i];
					fLength += fDiff * fDiff;
					pfTangent[iVertexIndex*3 + i] = fDiff;
				}

				if( 0.0f < fLength )
				{
					fLength = sqrtf(fLength);
					for(int i = 0; i < 3; i++)
						pfTangent[iVertexIndex*3 + i] /= fLength;
				}
			}
		}

		// assign tangents to the first and last vertices
		for(int i = 0; i < 3; i++)
		{
			pfTangent[(uCoordIndex ) * 3 + i] = 0.0f;
			pfTangent[(uCoordIndex + uNrOfVertices - 1) * 3 + i] = 0.0f;
		}

		// assign tangents to the first and last vertices
		// forward
		for(int iV = 0, iNonZeroLengthV = -1; iV < int(uNrOfVertices); iV++)
		{
			int iVertexIndex = int(uCoordIndex) + iV;

			float fLength = 0.0f;
			for(int i = 0; i < 3; i++)
			{
				float f = pfTangent[iVertexIndex*3 + i];
				fLength += f * f;
			}

			if( 0.0f < fLength )
			{
				iNonZeroLengthV = iVertexIndex;
			}
			else
			if( iNonZeroLengthV >= 0 )
				for(int i = 0; i < 3; i++)
				{
					pfTangent[iVertexIndex*3 + i] = pfTangent[iNonZeroLengthV*3 + i];
				}
		}

		// backward
		for(int iV = int(uNrOfVertices) - 1, iNonZeroLengthV = -1; iV >= 0 ; iV--)
		{
			int iVertexIndex = int(uCoordIndex) + iV;

			float fLength = 0.0f;
			for(int i = 0; i < 3; i++)
			{
				float f = pfTangent[iVertexIndex*3 + i];
				fLength += f * f;
			}

			if( 0.0f < fLength )
			{
				iNonZeroLengthV = iVertexIndex;
			}
			else
			if( iNonZeroLengthV >= 0 )
				for(int i = 0; i < 3; i++)
				{
					pfTangent[iVertexIndex*3 + i] = pfTangent[iNonZeroLengthV*3 + i];
				}
		}

		uCoordIndex += uNrOfVertices;

	}
	fclose(fpStreamline);

	for(int v = 0; v < int(uNrOfVertices); v++)
	{
		pfCoords[3 * v + 0] *= 2.0f / fScaleX;
		pfCoords[3 * v + 0] -= 1.0f;
		pfCoords[3 * v + 1] *= 2.0f / fScaleY;
		pfCoords[3 * v + 1] -= 1.0f;
		pfCoords[3 * v + 2] *= 2.0f / fScaleZ;
		pfCoords[3 * v + 2] -= 1.0f;
	}

	#if	RENDER_STREAMLINE_AS_LINES
	glGenBuffers(1, &vidLines);
	glBindBufferARB(GL_ARRAY_BUFFER, vidLines);
	glBufferDataARB(
		GL_ARRAY_BUFFER, 
		sizeof(pfCoords[0]) * pfCoords.USize() + 
			sizeof(pfTangent[0]) * pfTangent.USize() + 
			sizeof(pi4VertexIndicesInStreamline[0]) * pi4VertexIndicesInStreamline.USize(), 
		NULL, 
		GL_STATIC_DRAW_ARB);
	glBufferSubData(GL_ARRAY_BUFFER, 
		0, 
		sizeof(pfCoords[0]) * pfCoords.USize(), 
		&pfCoords[0]);
	glBufferSubData(GL_ARRAY_BUFFER, 
		sizeof(pfCoords[0]) * pfCoords.USize(), 
		sizeof(pfTangent[0]) * pfTangent.USize(), 
		&pfTangent[0]);
	glBufferSubData(GL_ARRAY_BUFFER, 
		sizeof(pfCoords[0]) * pfCoords.USize() + sizeof(pfTangent[0]) * pfTangent.USize(), 
		sizeof(pi4VertexIndicesInStreamline[0]) * pi4VertexIndicesInStreamline.USize(), 
		&pi4VertexIndicesInStreamline[0]);

	glBindBufferARB(GL_ARRAY_BUFFER, 0);
	#endif

	#if	RENDER_STREAMLINE_AS_TUBES
	_CreateTubes(0.01f);
	#endif

	for(int l = 0; l < int(uNrOfLines); l++)
	{
		double pdCoord_obj[3];
		for(int p = 0; p < 3; p++)
			pdCoord_obj[p] = 0.0;

		for(int v = 0; v < 2; v++)
		{
			int iP = pu2LineSegmentIndicesToVertices[l * 2 + v];
			for(int p = 0; p < 3; p++)
				pdCoord_obj[p] += pfCoords[3*iP + p];
		}

		for(int p = 0; p < 3; p++)
			pdCoord_obj[p] /= 2.0;

		for(int p = 0; p < 3; p++)
			pfLineCentroids[l * 3 + p] = pdCoord_obj[p];
	}

	#if	SORT_ON_CUDA	
	_ComputeDepthInit_cuda
	(
		int(uNrOfLines),
		&pfLineCentroids[0]
	);
	#endif	// #if	SORT_ON_CUDA
}

void 
CStreamline::_Render()
{
	#if	RENDER_STREAMLINE_AS_LINES

	_RenderLines();

	#endif

	#if	RENDER_STREAMLINE_AS_TUBES

	_RenderTubes();

	#endif

}

void 
CStreamline::_AddGlui(GLUI* pcGlui)
{
	GLUI_Panel	*pcPanel_Streamlines = pcGlui->add_rollout("Streamlines");

	GLUI_Spinner *pcSpinner_SamplingRate = pcGlui->add_spinner_to_panel(pcPanel_Streamlines, "Sampling Rate", GLUI_SPINNER_INT, &uSamplingRate);	
	pcSpinner_SamplingRate->set_int_limits(1, 256);

	GLUI_Spinner *pcSpinner_MinNrOfStreamlines = pcGlui->add_spinner_to_panel(pcPanel_Streamlines, "Min #Streamlines", GLUI_SPINNER_INT, &uMinNrOfStreamlines);	
		pcSpinner_MinNrOfStreamlines->set_int_limits(0, uMaxNrOfStreamlines);

	GLUI_Spinner *pcSpinner_MaxNrOfStreamlines = pcGlui->add_spinner_to_panel(pcPanel_Streamlines, "Max #Streamlines", GLUI_SPINNER_INT, &uMaxNrOfStreamlines);	
		pcSpinner_MaxNrOfStreamlines->set_int_limits(0, uMaxNrOfStreamlines);

	#if	RENDER_STREAMLINE_AS_LINES

	GLUI_Panel	*pcPanel_Lines = pcGlui->add_panel_to_panel(pcPanel_Streamlines, "Lines");
		GLUI_Spinner *pcSpinner_InnerWidth = pcGlui->add_spinner_to_panel(pcPanel_Lines, "Inner Width", GLUI_SPINNER_FLOAT, &fInnerWidth);	
		GLUI_Spinner *pcSpinner_OuterWidth = pcGlui->add_spinner_to_panel(pcPanel_Lines, "Outer Width", GLUI_SPINNER_FLOAT, &fOuterWidth);	

		GLUI_Panel	*pcPanel_Glyph = pcGlui->add_panel_to_panel(pcPanel_Lines, "Glyph");
		pcGlui->add_checkbox_to_panel(pcPanel_Glyph, "Enabled?", &cGlyph.ibIsEnabled);	
		GLUI_Spinner *pcSpinner_GlyphStep = pcGlui->add_spinner_to_panel(pcPanel_Glyph, "Step", GLUI_SPINNER_INT, &cGlyph.iStep);	
			pcSpinner_GlyphStep->set_int_limits(1, 128);
		GLUI_Spinner *pcSpinner_GlyphLength = pcGlui->add_spinner_to_panel(pcPanel_Glyph, "Length", GLUI_SPINNER_FLOAT, &cGlyph.fLength);	
			pcSpinner_GlyphLength->set_float_limits(0.0f, 16.0f);
		GLUI_Spinner *pcSpinner_GlyphWidth= pcGlui->add_spinner_to_panel(pcPanel_Glyph, "Width", GLUI_SPINNER_FLOAT, &cGlyph.fWidth);	
			pcSpinner_GlyphWidth->set_float_limits(0.0f, 16.0f);

		GLUI_Panel	*pcPanel_Dash = pcGlui->add_panel_to_panel(pcPanel_Lines, "Dash");
		GLUI_Spinner *pcSpinner_DashPeriod = pcGlui->add_spinner_to_panel(pcPanel_Dash, "Period", GLUI_SPINNER_INT, &cDash.iPeriod);	
			pcSpinner_DashPeriod->set_int_limits(0, 32);
		GLUI_Spinner *pcSpinner_DashOffset = pcGlui->add_spinner_to_panel(pcPanel_Dash, "Offset", GLUI_SPINNER_FLOAT, &cDash.fOffset);
			pcSpinner_DashOffset->set_float_limits(-M_PI, +M_PI);
		GLUI_Spinner *pcSpinner_DashThreshold = pcGlui->add_spinner_to_panel(pcPanel_Dash, "Threshold", GLUI_SPINNER_FLOAT, &cDash.fThreshold);
			pcSpinner_DashThreshold->set_float_limits(-1.0f, +1.0f);
		pcGlui->add_checkbox_to_panel(pcPanel_Dash, "Is Entropy Dependent?", &cDash.ibIsEntropyDependent);	
	#endif

	#if	RENDER_STREAMLINE_AS_TUBES
	GLUI_Panel	*pcPanel_Tubes = pcGlui->add_panel_to_panel(pcPanel_Streamlines, "Tubes");
		GLUI_Spinner *pcSpinner_Shininess = pcGlui->add_spinner_to_panel(pcPanel_Tubes, "Shininess", GLUI_SPINNER_INT, &cTubes.iShininess);	
		pcSpinner_Shininess->set_int_limits(0, 128);
		GLUI_Checkbox *pcCheckbox_IsWired = pcGlui->add_checkbox_to_panel(pcPanel_Tubes, "Wired?", &cTubes.ifIsWired);	
	#endif

	GLUI_Panel	*pcPanel_Color = pcGlui->add_panel_to_panel(pcPanel_Streamlines, "Color");
		GLUI_Spinner *pcSpinner_ColorR = pcGlui->add_spinner_to_panel(pcPanel_Color, "R", GLUI_SPINNER_FLOAT, &f4Color.x);	
		pcSpinner_ColorR->set_float_limits(0.0f, 1.0f);
		GLUI_Spinner *pcSpinner_ColorG = pcGlui->add_spinner_to_panel(pcPanel_Color, "G", GLUI_SPINNER_FLOAT, &f4Color.y);	
		pcSpinner_ColorG->set_float_limits(0.0f, 1.0f);
		GLUI_Spinner *pcSpinner_ColorB = pcGlui->add_spinner_to_panel(pcPanel_Color, "B", GLUI_SPINNER_FLOAT, &f4Color.z);	
		pcSpinner_ColorB->set_float_limits(0.0f, 1.0f);
		GLUI_Spinner *pcSpinner_ColorEdge = pcGlui->add_spinner_to_panel(pcPanel_Color, "Edge", GLUI_SPINNER_FLOAT, &f4Color.w);	
		pcSpinner_ColorEdge->set_float_limits(0.0f, 1.0f);
}

CStreamline::CStreamline(void)
{
	uSamplingRate = 1;

	uNrOfLines = 0;
	uNrOfVertices = 0;

	uLid = 0;

	fOuterWidth = 4.0f;
	fInnerWidth = 2.0f;

	f4Color.x = 0.1f;
	f4Color.y = 0.1f;
	f4Color.z = 0.1f;
	f4Color.w = 0.5f;
}

CStreamline::~CStreamline(void)
{
}

// ADD-BY-LEETY 2009/05/15-BEGIN
void 
CStreamline::_CreateTubes(float fTubeWidth)
{
	unsigned int uCoordBase = 0;
	unsigned int uLineIndex = 0;

	static unsigned int uNrOfSlices = 8;
	cTubes.pf3VertexCoords.alloc(uNrOfVertices * uNrOfSlices);
	cTubes.pf3VertexNormals.alloc(uNrOfVertices * uNrOfSlices);
	cTubes.puPatchIndices.alloc(uNrOfLines * 6 * uNrOfSlices);

	for(vector<unsigned int>::iterator 
			ivuNrOfVertices = vuNrOfVertices.begin();
		ivuNrOfVertices != vuNrOfVertices.end();
		ivuNrOfVertices ++)
	{
		unsigned int uNrOfVertices = *ivuNrOfVertices;

		for(unsigned int uV = 1; uV < uNrOfVertices-1; uV++)
		{
			Lib3dsVector v3P, v3C, v3N;
			for(int i = 0; i < 3; i++)
			{
				// P: previous; C: current; N: next
				v3P[i] = pfCoords[(uCoordBase + uV - 1	) * 3 + i];
				v3C[i] = pfCoords[(uCoordBase + uV		) * 3 + i];
				v3N[i] = pfCoords[(uCoordBase + uV + 1	) * 3 + i];
			}

			/*
			Lib3dsVector v3LP;
			lib3ds_vector_sub(v3LP, v3C, v3P);
			lib3ds_vector_normalize(v3LP);

			Lib3dsVector v3LN;
			lib3ds_vector_sub(v3LN, v3N, v3C);
			lib3ds_vector_normalize(v3LN);

			Lib3dsVector v3Normal;
			lib3ds_vector_cross(v3Normal, v3LP, v3LN);
			lib3ds_vector_normalize(v3Normal);
			*/
			Lib3dsVector v3LN;
			lib3ds_vector_sub(v3LN, v3N, v3C);
			lib3ds_vector_normalize(v3LN);

			static Lib3dsVector v3Z = {0.0f, 0.0f, 1.0f};

			Lib3dsVector v3Normal;
			lib3ds_vector_cross(v3Normal, v3LN, v3Z);
			lib3ds_vector_normalize(v3Normal);

			Lib3dsVector v3Up;
			lib3ds_vector_cross(v3Up, v3Normal, v3LN);
			lib3ds_vector_normalize(v3Up);


			lib3ds_vector_copy(cTubes.pf3VertexCoords[(uCoordBase + uV) * uNrOfSlices],  v3Normal);
			lib3ds_vector_copy(cTubes.pf3VertexCoords[(uCoordBase + uV) * uNrOfSlices + 1],  v3Up);
			if( uV == 1 )
			{
				lib3ds_vector_copy(cTubes.pf3VertexCoords[uCoordBase * uNrOfSlices],  v3Normal);
				lib3ds_vector_copy(cTubes.pf3VertexCoords[uCoordBase * uNrOfSlices + 1],  v3Up);
			}

			if( uV == uNrOfVertices - 1 )
			{
				lib3ds_vector_copy(cTubes.pf3VertexCoords[(uCoordBase + uV + 1)* uNrOfSlices],  v3Normal);
				lib3ds_vector_copy(cTubes.pf3VertexCoords[(uCoordBase + uV + 1)* uNrOfSlices + 1],  v3Up);
			}
		}

		for(unsigned int uV = 0; uV < uNrOfVertices; uV++)
		{
			Lib3dsVector v3C;
			for(int i = 0; i < 3; i++)
			{
				// P: previous; C: current; N: next
				v3C[i] = pfCoords[(uCoordBase + uV) * 3 + i];
			}

			Lib3dsVector v3Up;
			lib3ds_vector_copy(v3Up,		cTubes.pf3VertexCoords[(uCoordBase + uV) * uNrOfSlices]);

			Lib3dsVector v3Normal;
			lib3ds_vector_copy(v3Normal,	cTubes.pf3VertexCoords[(uCoordBase + uV) * uNrOfSlices + 1]);

			for(int i = 0; i < (int)uNrOfSlices; i++)
			{
				float fAngle = (float) i * 2.0f * (float)M_PI / (float)uNrOfSlices;

				Lib3dsVector v3X;
				lib3ds_vector_copy(v3X, v3Normal);
				lib3ds_vector_scalar(v3X, cosf(fAngle));

				Lib3dsVector v3Y;
				lib3ds_vector_copy(v3Y, v3Up);
				lib3ds_vector_scalar(v3Y, sinf(fAngle));

				Lib3dsVector v3Dir;
				lib3ds_vector_add(v3Dir, v3X, v3Y);
				lib3ds_vector_scalar(v3Dir, fTubeWidth);

				Lib3dsVector v3Coord;
				lib3ds_vector_add(v3Coord, v3C, v3Dir);

				lib3ds_vector_copy(cTubes.pf3VertexCoords[(uCoordBase + uV) * uNrOfSlices + i],  v3Coord);

				// calculate the normals
				Lib3dsVector v3VertexNormal;
				lib3ds_vector_sub(v3VertexNormal, v3Coord, v3C);
				lib3ds_vector_normalize(v3VertexNormal);

				lib3ds_vector_copy(cTubes.pf3VertexNormals[(uCoordBase + uV) * uNrOfSlices + i],  v3VertexNormal);
			}
		}

		for(unsigned int uV = 0; uV < uNrOfVertices - 1; uV++, uLineIndex++)
		{
			for(int i = 0; i < (int)uNrOfSlices - 1; i++)
			{
				static int puPatchOffset[] = 
				{
					0, uNrOfSlices,		uNrOfSlices+1, 
					0, uNrOfSlices + 1,	1, 
				};
				for (int j = 0; j < 6; j++)
				{
					cTubes.puPatchIndices[uLineIndex * 6 * uNrOfSlices + i * 6 + j] = 
						(uCoordBase + uV) * uNrOfSlices + i + puPatchOffset[j];
				}
			}
			static int puPatchOffset[] = 
			{
				uNrOfSlices - 1, 2 *uNrOfSlices - 1, uNrOfSlices, 
				uNrOfSlices - 1, uNrOfSlices, 0, 
			};
			for (int j = 0; j < 6; j++)
			{
				cTubes.puPatchIndices[uLineIndex * 6 * uNrOfSlices + (uNrOfSlices - 1) * 6 + j] = 
					(uCoordBase + uV) * uNrOfSlices + puPatchOffset[j];
			}
		}

		uCoordBase += uNrOfVertices;
	}
	glVertexPointer(3, GL_FLOAT, 0, &cTubes.pf3VertexCoords[0]);
	glNormalPointer(GL_FLOAT, 0, &cTubes.pf3VertexNormals[0]);

	uLid = glGenLists(1);
	glNewList(uLid, GL_COMPILE);

	glPushMatrix();

	glPushAttrib(GL_ENABLE_BIT);
	glPushAttrib(GL_LINE_BIT);
	glPushAttrib(GL_POLYGON_BIT);
	glPushAttrib(GL_DEPTH_BUFFER_BIT);
	glPushAttrib(GL_COLOR_BUFFER_BIT);
	glPushAttrib(GL_LIGHTING_BIT);

	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	glEnable(GL_CULL_FACE);

	glEnable(GL_LIGHTING);
	glEnable(GL_NORMALIZE);
	glEnable(GL_LIGHT0);

	glShadeModel(GL_SMOOTH);
	glFrontFace(GL_CW);

	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_NORMAL_ARRAY);

	glDrawElements(
		GL_TRIANGLES, 
		cTubes.puPatchIndices.USize(), 
		GL_UNSIGNED_INT, 
		&cTubes.puPatchIndices[0]);

	glDisableClientState(GL_NORMAL_ARRAY);
	glDisableClientState(GL_VERTEX_ARRAY);

	glPopAttrib();	// glPushAttrib(GL_POLYGON_BIT);
	glPopAttrib();	// glPushAttrib(GL_DEPTH_BUFFER_BIT);
	glPopAttrib();	// glPushAttrib(GL_COLOR_BUFFER_BIT);
	glPopAttrib();	// glPushAttrib(GL_LINE_BIT);
	glPopAttrib();	// glPushAttrib(GL_LIGHTING_BIT);
	glPopAttrib();	// GLPushAttrib(GL_ENABLE_BIT);

	glPopMatrix();
	glEndList();
}

void
CStreamline::_RenderLinesInSlab
(
	int iSlab,
	bool bDrawHalo 
)
{
	if( iSlab >= int(pi2BaseLengths.USize()) )
	{
		LOG(printf("Warning!"));
		return;
	}

	if( 0 == pi2BaseLengths[iSlab].y )
		return;

	glPushAttrib(GL_CURRENT_BIT);
	glPushAttrib(GL_LINE_BIT);
	glPushAttrib(GL_DEPTH_BUFFER_BIT);
	glPushClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT);

	glBindBuffer(GL_ARRAY_BUFFER, vidLines);

	glVertexPointer(3, GL_FLOAT, 0, (void*)0);	// &pfCoords[0]);
	glEnableClientState(GL_VERTEX_ARRAY);

	glNormalPointer(GL_FLOAT, 0,	(void*)(pfCoords.USize() * sizeof(pfCoords[0])));
	glEnableClientState(GL_NORMAL_ARRAY);

	glClientActiveTexture(GL_TEXTURE0 + 1);
	glTexCoordPointer(4, GL_INT, 0, (void*)(pfCoords.USize() * sizeof(pfCoords[0]) + pfTangent.USize() * sizeof(pfTangent[0])));
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);

	if( bDrawHalo )
	{
		glLineWidth(fOuterWidth);
		glColor4f(
			f4Color.x * f4Color.w, 
			f4Color.y * f4Color.w, 
			f4Color.z * f4Color.w, 
			1.0f);
		glDrawElements(
			GL_LINES, 
			2*pi2BaseLengths[iSlab].y, // pu2LineSegmentIndicesToVertices.USize(), 
			GL_UNSIGNED_INT, 
			&pu2SortedLineSegmentIndicesToVertices[2*pi2BaseLengths[iSlab].x]); // &pu2LineSegmentIndicesToVertices[0]);
	}
	else
	{
	
	glLineWidth(fInnerWidth);
	glColor4f(
		f4Color.x, 
		f4Color.y, 
		f4Color.z, 
		1.0f);
	glDrawElements(
		GL_LINES, 
		2*pi2BaseLengths[iSlab].y, // pu2LineSegmentIndicesToVertices.USize(), 
		GL_UNSIGNED_INT, 
		&pu2SortedLineSegmentIndicesToVertices[2*pi2BaseLengths[iSlab].x]); // &pu2LineSegmentIndicesToVertices[0]);

	}

	glBindBuffer(GL_ARRAY_BUFFER, 0);

	glPopClientAttrib();	// glPushClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT);

	glPopAttrib();	// glPushAttrib(GL_DEPTH_BUFFER_BIT);
	glPopAttrib();	// glPushAttrib(GL_LINE_BIT);
	glPopAttrib();	// glPushAttrib(GL_CURRENT_BIT);
}

void
CStreamline::_RenderLines()
{
	glPushAttrib(GL_CURRENT_BIT);
	glPushAttrib(GL_LINE_BIT);
	glPushAttrib(GL_DEPTH_BUFFER_BIT);

	glEnableClientState(GL_VERTEX_ARRAY);

	glDepthFunc(GL_LEQUAL);
	glLineWidth(fOuterWidth);
	glColor4f(
		f4Color.x * f4Color.w, 
		f4Color.y * f4Color.w, 
		f4Color.z * f4Color.w, 
		1.0f);
	glDrawElements(
		GL_LINES, 
		pu2LineSegmentIndicesToVertices.USize()/128,
		GL_UNSIGNED_INT, 
		&pu2LineSegmentIndicesToVertices[0]);

	glLineWidth(fInnerWidth);
	glColor4f(
		f4Color.x, 
		f4Color.y, 
		f4Color.z, 
		1.0f);
	glDrawElements(
		GL_LINES, 
		pu2LineSegmentIndicesToVertices.USize(), 
		GL_UNSIGNED_INT, 
		&pu2LineSegmentIndicesToVertices[0]);

	glDisableClientState(GL_VERTEX_ARRAY);

	glPopAttrib();	// glPushAttrib(GL_DEPTH_BUFFER_BIT);
	glPopAttrib();	// glPushAttrib(GL_LINE_BIT);
	glPopAttrib();	// glPushAttrib(GL_CURRENT_BIT);
}

void
CStreamline::_RenderTubes()
{
	glPushAttrib(GL_POLYGON_BIT);
	glPushAttrib(GL_LIGHTING_BIT);

    static GLfloat pfLightAmbient[4] =	{0.0f, 0.0f, 0.0f, 1.0f};
	static GLfloat pfLightColor[4] =	{0.5f, 0.5f, 0.5f, 1.0f};
    static GLfloat pfLightPos[4] =		{0.0f, 0.0f, 0.0f, 1.0f};
	glLightfv(GL_LIGHT0, GL_AMBIENT,	pfLightAmbient);
	glLightfv(GL_LIGHT0, GL_DIFFUSE,	pfLightColor);
	glLightfv(GL_LIGHT0, GL_SPECULAR,	pfLightColor);
	
	glPushMatrix();
	glLoadIdentity();
	glLightfv(GL_LIGHT0, GL_POSITION, pfLightPos);
	glPopMatrix();

	static GLfloat mat_amb[] = { 0.0f, 0.0f, 0.0f, 1.0f };
	static GLfloat mat_diffuse[] = { 0.4f, 0.4f, 0.4f, 1.0f };
	static GLfloat mat_specular[] = { 0.4f, 0.4f, 0.4f, 1.0f };
	mat_diffuse[0] = f4Color.x;
	mat_diffuse[1] = f4Color.y;
	mat_diffuse[2] = f4Color.z;
	mat_specular[0] = f4Color.x * f4Color.w;
	mat_specular[1] = f4Color.y * f4Color.w;
	mat_specular[2] = f4Color.z * f4Color.w;
	glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, mat_amb);
	glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, mat_diffuse);
	glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, mat_specular);
	glMateriali(GL_FRONT_AND_BACK, GL_SHININESS, cTubes.iShininess);

	if( cTubes.ifIsWired )
		glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

	glColor4f(
		f4Color.x * f4Color.w, 
		f4Color.y * f4Color.w, 
		f4Color.z * f4Color.w, 
		1.0f);

	glCallList(uLid);

	glPopAttrib();	// glPushAttrib(GL_POLYGON_BIT);
	glPopAttrib();	// glPushAttrib(GL_LIGHTING_BIT);
}

/*

$Log: not supported by cvs2svn $
Revision 1.11  2010/03/10 20:21:59  leeten

[03/10/2010]
1. [ADD] Add a new variable uMinNrOfStreamlines. Only the streamlines whose ids are larger than uMinNrOfStreamlines will be rendered. A new spinner is added to control it.

Revision 1.10  2010/02/05 01:44:56  leeten

[02/04/2010]
1. [ADD] Add a new preprocessor SORT_ON_CUDA. If it is 0, the sorting is done via CPU-side bucket sort.
2. [MOD] Change the CPU-side sorting from quick sort to bucket sort.

Revision 1.9  2010/02/02 03:51:23  leeten

[02/01/2010]
1. [MOD] Mark the date to add the header "cuda_macro.h"

Revision 1.8  2010/02/01 06:08:55  leeten

[01/31/2010]
1. [ADD] include the header cuda_macro.h
2. [MOD] if the preprocessor __DEVICE_EMULATION__ is defined, sort the seeds on CPU instread.
3. [MOD] Use only one vector product to compute the depth in the eye space.
4. [MOD] Use VBO to render the streamlines.

Revision 1.7  2010/01/19 21:12:13  leeten

[01/19/2010]
1. [MOD] Extend the 2nd texture cooridnate from scalars to 4-tuple vectors and setup the t component in the 2nd texture coordinate to specify the streamline index.

Revision 1.6  2010/01/12 23:51:12  leeten

[01/12/2010]
1. [ADD] Add the user control to control the paramters for flyph drawing.

Revision 1.5  2010/01/11 19:22:30  leeten

[01/10/2010]
1. [ADD] Include the header glew.h.
2. [ADD] Add an array piVertexIndicesInStreamline to record the index of each vertice along its streamline. These indices will be passed to the 1st textre unit.
3. [ADD] Add a panel to control the styles of the dashed lines.
4. [ADD] Use glPushClientAttrib()/glPopClientAttrib to disable the enabled vertex arrays.

Revision 1.4  2010/01/09 22:19:12  leeten

[01/09/2010]
1. [ADD] Define a new field uMaxNrOfStreamlines as max. #streamlines to be rendered.

Revision 1.3  2010/01/04 18:32:59  leeten

[01/04/2010]
1. [ADD] Allow the change of the sampling rate of streamlines when rendering. In order to do this, an array puLineSegmentIndicesToStreamlines is added to recorded the streamline indices of all streamline segments.
2. [MOD] Change the initial value of the line color to (0.1, 0.1, 0.1).

Revision 1.2  2010/01/01 18:31:11  leeten

[01/01/2010]
1. [ADD] Calculate the tangent vectors when loading the streamlines.
2. [MOD] Change the function _RenderLinesInSlab so every time only either the outter or inter line will be rendered.

Revision 1.1  2009/12/31 01:53:59  leeten

[12/30/2009]
1. [1ST] First time checkin.

Revision 1.3  2009/05/18 23:28:29  leeten

[2009/05/18]
1. [ADD] Force the front face to be GL_CW.

Revision 1.2  2009/05/15 20:41:09  leeten

[2009/05/15]
1. [ADD] Add a preprocessor RENDER_STREAMLINE_AS_LINES to render the streamlins as line segments.
2. [MOD] Move the vector of unsigned int vuNrOfVertices to the class.
3. [MOD] Define a member method _RenderLines()/_RenderTubes() as the code segment to render the streamlines as line segments and the tubes, respectivelty.
4. [MOD] Move the creation of streamlines as 3D tube to the new member method _CreateTuves().
5. [MOD] Redefine the method _AddGlui() to group related variavbles into panels.

Revision 1.1  2009/05/12 18:54:26  leeten

[2009/05/12]
1. [1ST] This project defines an GLUT/GLUI viewer to display the PRIs.


*/
