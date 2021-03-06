#include "pipeline.h"

void PuresoftPipeline::drawVAO(int vao, bool callerThrdForFragProc /* = false */)
{
#ifdef PROFILING
	for(int i = 0; i < m_numberOfThreads; i++)
	{
		m_fragTaskQueues[i].resetCounters();
	}
#endif

	if(!m_vp || !m_ip || !m_fp || vao < 0 || vao >= (int)m_vaoPool.size())
	{
		return;
	}

	PuresoftVAO* vaoObj = m_vaoPool[vao];
	m_vp->preprocess(m_uniforms);
	m_ip->preprocess(m_uniforms);
	m_fp->preprocess(m_uniforms, (const void**)&m_texPool[0]);

	// reset vertex pointers of all vbos
	vaoObj->rewindAll();

	__declspec(align(16)) float correctionFactor1[4] = {0};
	__declspec(align(16)) float projZs[4] = {0};
	PuresoftInterpolater::INTERPOLATIONSTARTSTEP interp;
	interp.proc = m_ip;
	interp.vertexUserData[0] = m_vertOutput[0].user;
	interp.vertexUserData[1] = m_vertOutput[1].user;
	interp.vertexUserData[2] = m_vertOutput[2].user;
	interp.vertices = (const float*)m_rasterResult->vertices;
	interp.reciprocalWs = correctionFactor1;
	interp.projectedZs = projZs;

	while(true)
	{
		// vertex transformation
		if(!processVertices(vaoObj->getVBOs(), m_vertOutput, correctionFactor1, projZs))
		{
			// done with all vertices
			break;
		}

		// cull back-face
		if((m_behavior & BEHAVIOR_FACE_CULLING) && isBackFace(m_vertOutput[0].position, m_vertOutput[1].position, m_vertOutput[2].position))
		{
			continue;
		}

		if(m_vertOutput[0].position[2] < -1.0f || m_vertOutput[0].position[2] > 1.0f)
			continue;
		if(m_vertOutput[1].position[2] < -1.0f || m_vertOutput[1].position[2] > 1.0f)
			continue;
		if(m_vertOutput[2].position[2] < -1.0f || m_vertOutput[2].position[2] > 1.0f)
			continue;

		// rasterization
		if(!m_rasterizer.pushTriangle(m_vertOutput[0].position, m_vertOutput[1].position, m_vertOutput[2].position))
		{
			// 0 sized triangle
			continue;
		}

		// interpolation + fragment filling
		for(interp.row = m_rasterResult->firstRow; interp.row <= m_rasterResult->lastRow; interp.row++)
		{
			// get one row of the rasterized triangle
			PuresoftRasterizer::RESULT_ROW& row = m_rasterResult->m_rows[interp.row];

			// skip zero length row
			if(row.left == row.right)
			{
				continue;
			}

			// partitioning (determine which fragment thread to process this row)
			int threadIndex;
			
			if(callerThrdForFragProc)
				threadIndex = interp.row % m_numberOfThreads;
			else
				threadIndex = interp.row % (m_numberOfThreads - 1);

			FragmentThreadTaskQueue* taskQueue = m_fragTaskQueues + threadIndex;

			// interpolation (preparation of per-fragment interpolation)
			FRAGTHREADTASK* newTask = taskQueue->beginPush();
			interp.leftColumn = row.left;
			interp.leftColumnSkipping = row.leftClamped - row.left;
			interp.rightColumn = row.right;
			interp.leftVerts = row.leftVerts;
			interp.rightVerts = row.rightVerts;
			interp.interpolatedUserDataStart = newTask->userDataStart;
			interp.interpolatedUserDataStep = newTask->userDataStep;
			m_interpolater.interpolateStartAndStep(&interp);

			// complete the task item
			newTask->taskType = DRAW;
			newTask->x1 = row.leftClamped;
			newTask->x2 = row.rightClamped;
			newTask->y = interp.row;
			newTask->projZStart = interp.projectedZStart;
			newTask->projZStep = interp.projectedZStep;
			newTask->correctionFactor2Start = interp.correctionFactor2Start;
			newTask->correctionFactor2Step = interp.correctionFactor2Step;

			// push to fragment thread
			taskQueue->endPush();
		}
	}

	if(callerThrdForFragProc)
	{
		FragmentThreadTaskQueue* taskQueue = m_fragTaskQueues + m_numberOfThreads - 1;
		taskQueue->beginPush()->taskType = QUIT;
		taskQueue->endPush();
		fragmentThread_CallerThread(this);
	}

	// wait for all fragment threads to finish up tasks
	for(int i = 0; i < m_numberOfThreads - 1; i++)
	{
		FragmentThreadTaskQueue* taskQueue = m_fragTaskQueues + i;
		taskQueue->beginPush()->taskType = ENDOFDRAW;
		taskQueue->endPush();
	}

	for(int i = 0; i < m_numberOfThreads - 1; i++)
	{
		m_fragTaskQueues[i].pollEmpty();
	}
}