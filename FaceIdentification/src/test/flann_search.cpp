#include <fstream>
#include <iostream>
#include <string>
#include <time.h>
#include <stdio.h>
#include <flann/flann.hpp>

#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <zlib.h>
#include <sys/timeb.h>
#include "gcommon.h"
#include "picsearch.h"

#include <opencv2/opencv.hpp> 
#include<opencv2/highgui/highgui.hpp> 
using namespace flann;
using namespace std;

#define MODEL_LIST "idata/bsfeats_0.list"
#define TEST_LIST "idata/linyongjian.list"
#define MODE_FEATURE "idata/bsfeats_0.dat"
#define MODE_INDEXER "idata/bsfeats_0.dat.flannidx"
std::string DATA_DIR = "./data/";
#include <xmmintrin.h>

#ifdef _WIN32
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

int g_Argc;
char **g_Argv;
map<string, string> g_mapConf;    //config infmation
flann::Index<flann::L2<float> > * g_pflann_index;	  //flann近邻查找索引
flann::Matrix<float>  g_FeatureDataset; 	//未压缩的特征矩阵
vector<string> g_vecImage;		//图片信息
int g_nImageCount;		//图片数量
int g_flann_max_search= 10000;	//近邻搜索时检查节点的最大数量


vector<string> g_testImage;
int g_nTestImageCount;

bool bInit();
bool bGetFileList(const string &strCMD, vector<string> &vecFNameList);
bool bGetIdx(const string &strSrc);
bool bZipData(const string &strSrc);
bool bInitConf();
bool bLoad(Matrix<float> &, const string&);


void vPrintTimeLast(struct timeb * tpOld, const char * ch)
{
	struct timeb tNow;
	ftime(&tNow);

	int nSec= tNow.time - tpOld->time;
	int nMsec= tNow.millitm -tpOld->millitm;
	if(nMsec<0)
	{	
		nSec-=1;
		nMsec=1000+nMsec;
	}
	//printf("using %d s and %d ms\n", nSec, nMsec);
	LOG(INFO)<<ch<<" Using:"<< nSec<<"s "<<nMsec<<"ms";
}

float simd_dot(const float* x, const float* y, const long& len) {
  float inner_prod = 0.0f;
  __m128 X, Y; // 128-bit values
  __m128 acc = _mm_setzero_ps(); // set to (0, 0, 0, 0)
  float temp[4];

  long i;
  for (i = 0; i + 4 < len; i += 4) {
      X = _mm_loadu_ps(x + i); // load chunk of 4 floats  取4位x+i[0,1,2,3]
      Y = _mm_loadu_ps(y + i);
      acc = _mm_add_ps(acc, _mm_mul_ps(X, Y)); //_mm_mul_ps(x0*y0, x1*y1, x2*y2, x3*y3) 4个依次存入acc中，累加
  }
  _mm_storeu_ps(&temp[0], acc); // store acc into an array  将acc累加内容存入temp数组
  inner_prod = temp[0] + temp[1] + temp[2] + temp[3];  //求和

  // add the remaining values
  for (; i < len; ++i) {
      inner_prod += x[i] * y[i];
  }
  return inner_prod;
}


float CalcSimilarity(float* const fc1,
    float* const fc2,
    long dim) {
  if (dim == -1) {
    dim = 2048;
  }
  return simd_dot(fc1, fc2, dim)
	  / (sqrt(simd_dot(fc1, fc1, dim))
	  * sqrt(simd_dot(fc2, fc2, dim)));
}

//ImageId : need to modify
bool bInitImageInfo()
{
	
	//读取文件
	//FILE* fd= fopen("idata/china1.feature","r"); 
	FILE* fd= fopen(MODEL_LIST,"r");
        char sline[1024];
        int num = 0;
        while(fgets(sline,sizeof(sline),fd))
        {
            if(NULL == sline)
                break;
            sline[strlen(sline)-1] = 0;
	     char tmp[1024];
	    char * token = strtok(sline, "\t");  
            strcpy(tmp, token); 
	    g_vecImage.push_back(tmp);
        }     
	fclose(fd);

        for(int i = 0; i< 10; i++) 
        {
            std::cout<<g_vecImage[i];
        }	
	g_nImageCount= g_vecImage.size();
	std::cout<<"g_nImageCount "<<g_nImageCount<<std::endl;
	return true;	
}

//ImageId : need to modify
bool bInitTestImageInfo()
{
        FILE* fd= fopen(TEST_LIST,"r");
        char sline[1024];
        int num = 0;
        while(fgets(sline,sizeof(sline),fd))
        {
            if(NULL == sline)
                break;
            sline[strlen(sline)-1] = 0;
             char tmp[1024];
            char * token = strtok(sline, "\t");
            strcpy(tmp, token);
            g_testImage.push_back(tmp);
        }
        fclose(fd);

        g_nTestImageCount= g_testImage.size();
        std::cout<<"g_nTestImageCount "<< g_nTestImageCount<<std::endl;
        return true;
}

bool bInitFlannIndex_uncompress()
{
	int fd;
	struct stat fs;

	//读取特征文件
	//fd= open("idata/bsfeats_1.dat",O_RDWR);
	fd= open(MODE_FEATURE,O_RDWR);  
	if(-1==fd)
	{
		perror("read feature file error");
		return false;
	} 

	if(-1==fstat(fd, &fs))
	{
		perror("fstate feature file error");
		close(fd);
		return false;
	}
	size_t sizeFileLen= fs.st_size;
	size_t sizeUnitLen= sizeof(float)*2048;
	size_t sizeImageNum= sizeFileLen/sizeUnitLen;

    	//分配内存，拷贝数据
    	int nUnitsPerRead = 100;//批量读取，加快初始化速度
	char *p= new char[sizeUnitLen*nUnitsPerRead+1];
    	g_FeatureDataset= flann::Matrix<float>(new float[sizeFileLen/sizeof(float)], sizeImageNum, 2048);
	size_t i=0;
	while(i<sizeImageNum)
	{
		size_t nReadLen=0;
		memset(p, 0, sizeUnitLen*nUnitsPerRead+1);
		
		size_t nUnits= nUnitsPerRead;
		if(sizeImageNum-i<nUnitsPerRead)	
			nUnits= sizeImageNum-i;
		while(nReadLen<nUnits*sizeUnitLen)
		{
			int nRet= read(fd, p+nReadLen, (nUnits*sizeUnitLen-nReadLen));
			if(nRet==-1)
			{
				perror("read feature file error");
				close(fd);
				delete []p;
				return false;
			}
			nReadLen+=  nRet;	
		}
		memcpy(g_FeatureDataset[i], p, nReadLen);
		i+=nUnits;
	}


        //normalize the inputData:
        float norm = 0.0;
        for(int j = 0; j< sizeImageNum; j++) {
            for(int i = 0; i<IMAGE_FEATURE_NUM; i++) {
               norm += g_FeatureDataset[j][i] * g_FeatureDataset[j][i];
            }
            norm = sqrt(norm);
            if(j == 0)
                LOG(INFO)<<"SEARCH INPUT norm is "<<norm;

            for(int i = 0; i<IMAGE_FEATURE_NUM; i++) {
                g_FeatureDataset[j][i] /= norm;
            }
        }

        /*for(int i = 0; i<IMAGE_FEATURE_NUM; i++) {
            LOG(INFO)<<" g_FeatureDataset  "<< g_FeatureDataset[0][i]<<" ";
        }*/

	g_pflann_index= new flann::Index<flann::L2<float> > (g_FeatureDataset, 
				flann::SavedIndexParams(MODE_INDEXER) );	
	return true;
}

int resize(int cols, int rows)
{
    int proportion = 1;
    if (cols > rows)
        proportion = rows/640 +1;
    else
        proportion = cols/640 +1;
    return proportion;
}

int main(int argc, char **argv)
{
    std::string test_dir = DATA_DIR + "test_face_recognizer/";
    /*show */
    cv::namedWindow("test",cv::WINDOW_NORMAL);
    cv::namedWindow("test1",cv::WINDOW_NORMAL);
	int proportion = 1;
	cv::Mat resize_img;

    g_Argc= argc;
    g_Argv= argv;
    vector<string> vecFNameList;

    if(!bInit())
    {
        LOG(ERROR)<<"Init fail";
        return -1;
    }

	//test image whole file name
    if (!bInitImageInfo())
    {
	LOG(ERROR)<<"Init Imagelist fail";
        return -1;
    }

    //init test image path
    if (!bInitTestImageInfo())
    {
        LOG(ERROR)<<"Init test Imagelist fail";
        return -1;
    }

    //加载待检测的特征
    Matrix<float> mTestset;
    string strTest = argv[2];
    
    if(!bLoad(mTestset, strTest))
    {
        LOG(ERROR)<<"Load File Error:"<<strTest;
        return false;
    }
    std::vector<float*> test_points; 
    int size_ = mTestset.rows;
    int veclen_ = mTestset.cols;

    test_points.resize(size_);
    for (size_t i=0;i<size_;++i) {
        test_points[i] = mTestset[i];
    }

    std::cout<<"test (rows,cols) is " << size_<< " "<<veclen_<<std::endl;

    //加载idx
    if(!bInitFlannIndex_uncompress())
    {
        LOG(ERROR)<<"Init Feature Error";
        return false;
    }
    LOG(INFO)<<"after Read feat file";

    int nn = 3;
    float fFeatures[2048] = {0.0};
    //query对象
    flann::Matrix<float> flann_matrix_query(new float[2048], 1, 2048); 
    flann::Matrix<int> flann_matrix_indices(new int[3], 1, nn);
    flann::Matrix<float> flann_matrix_dists(new float[3], 1, nn);
    memset(flann_matrix_indices.ptr(), 0, 3*sizeof(int));
    memset(flann_matrix_dists.ptr(), 0, 3*sizeof(float));
    memset(flann_matrix_query.ptr(), 0, 2048*sizeof(float));

    /*for(int i = 0; i<IMAGE_FEATURE_NUM; i++) {
            LOG(INFO)<<test_points[0][i]<<" ";
        }*/

    //normalize the inputData:
    float norm = 0.0;
    for(int j = 0; j< size_; j++) {
        for(int i = 0; i<IMAGE_FEATURE_NUM; i++) {
           norm += test_points[j][i] * test_points[j][i];
        }
        norm = sqrt(norm);
        if(j == 0)
            LOG(INFO)<<"norm is "<<norm;

        for(int i = 0; i<IMAGE_FEATURE_NUM; i++) {
           test_points[j][i] /= norm;
        }
    }

        /*for(int i = 0; i<IMAGE_FEATURE_NUM; i++) {
            LOG(INFO)<< test_points[0][i]<<" ";
        }*/
    //size_ = 8;
    for (int i = 0; i < size_; i ++) {
	    //show test image
        std::cout<<"input image path is "<<test_dir+g_testImage[i]<<std::endl;
	    cv::Mat input_image = cv::imread(test_dir+g_testImage[i],1);
		proportion = resize(input_image.cols, input_image.rows);
		std::cout<<"proportion "<< proportion<<std::endl;
		cv::resize(input_image,resize_img,cv::Size( input_image.cols/proportion, input_image.rows/proportion),(0,0),(0,0),3);  
		std::cout<<"input_image(cols, rows) "<< input_image.cols<<", " << input_image.rows <<std::endl; 
		std::cout<<"resize_img(cols, rows) "<< resize_img.cols<<", " << resize_img.rows <<std::endl; 	    
		cv::imshow("test", resize_img);
        cv::waitKey(300);
	
        struct timeb t;
        ftime(&t); 
        memcpy(flann_matrix_query.ptr(), test_points[i], 2048*sizeof(float));
    
        int nResultSize= g_pflann_index->knnSearch(flann_matrix_query, flann_matrix_indices, flann_matrix_dists, nn, flann::SearchParams(g_flann_max_search));

        for(int i=0; i<nResultSize; i++)
        {
            int nImageId= flann_matrix_indices[0][i];
    	    std::cout<<i << " ,nImageId "<<nImageId <<" "<< g_vecImage[nImageId]<< " "<<flann_matrix_dists[0][i]<<std::endl;
            //std::cout<<i << " "<< nImageId<< " "<<flann_matrix_dists[0][i];

            std::cout<<"output image path is "<<test_dir+g_vecImage[nImageId]<<std::endl;
		    cv::Mat output_image = cv::imread(test_dir+g_vecImage[nImageId], 1);
			proportion = resize(output_image.cols, output_image.rows);
			std::cout<<"proportion "<< proportion<<std::endl;
		    cv::resize(output_image,resize_img,cv::Size(output_image.cols/proportion, output_image.rows/proportion),(0,0),(0,0),3);
			std::cout<<"output_image(cols, rows) "<< output_image.cols<<", " << output_image.rows <<std::endl; 
			std::cout<<"resize_img(cols, rows) "<< resize_img.cols<<", " << resize_img.rows <<std::endl; 
			cv::imshow("test1", resize_img);
            cv::waitKey(0);
        }
        
        //std::cout<<"output image path is "<<test_dir+g_vecImage[flann_matrix_indices[0][0]];
		//cv::Mat input_image1 = cv::imread(test_dir+g_vecImage[flann_matrix_indices[0][0]],1);
		//cv::imshow("test1", input_image1);
        //cv::waitKey(300000);
        std::cout<<std::endl;
        vPrintTimeLast(&t, "一次计算耗时 ");
//}

    //use method 2 check distance
    std::cout<< "g_FeatureDataset(rows,cols) is " <<g_FeatureDataset.rows <<","<< g_FeatureDataset.cols<<std::endl;
    //ftime(&t);
    int max_index = 0;
    float max_sim = 0.0;
    for(int j = 0; j<g_FeatureDataset.rows; j++) {
        //float sim = CalcSimilarity(mTestset[1], g_FeatureDataset[j],2048);
        float sim = CalcSimilarity(mTestset[i], g_FeatureDataset[j],2048);
        if (max_sim < sim) {
            max_sim = sim;
            max_index = j;
	}
        //std::cout <<i <<"  "<< sim <<endl;
    }
    //vPrintTimeLast(&t, "数得分胜多负少 ");
    cout<< "max_index "<< max_index <<" max_sim "<< max_sim <<endl;

}
    delete[] mTestset.ptr();
    cv::destroyWindow("test");
	cv::destroyWindow("test1");
    return 0;
    
}

bool bInit()
{
    //init log
    google::InitGoogleLogging(g_Argv[0]);
    FLAGS_log_dir= "./log";
    FLAGS_max_log_size=10; //设置日志文件最大10M
    FLAGS_logbufsecs = 0; //输出日志延迟， 默认为30秒，现在设置为实时
    FLAGS_stderrthreshold= google::INFO;

    if(g_Argc < 2)
    {
        LOG(ERROR)<<"usage: "<< g_Argv[0]<<" configfile";
        return false;
    }
    if(!bInitConf())
    {
        LOG(ERROR)<<"Init config fail";
        return false;
    }

    return true;
}

bool bInitConf()
{
    //config file name
    char *szConfigFile = g_Argv[1];

    char chProfile[512];
    char chSection[512];
    char chEntry[512];
    char chValue[512];
    memset(chProfile, 0, 512);
    memset(chSection, 0, 512);
    memset(chEntry, 0, 512);
    memset(chValue, 0, 512);

    //FeZipCMD
    snprintf(chProfile, 512, szConfigFile);
    snprintf(chSection, 512, "Merge");
    snprintf(chEntry, 512, "FeZipCMD");
    if(GetProfileString(chProfile, chSection, chEntry, "", chValue, 512)==0)
    {
        LOG(ERROR)<< "Read config Error, Section="<<chSection<<", Entry=" <<chEntry;
        return false;
    }
    g_mapConf.insert(make_pair<string, string>(chEntry, chValue));

    return true;
}



bool bGetIdx(const string &strSrc)
{
    Matrix<float> mDataset;
    string strIdxName=strSrc+".flannidx";
    if(!bLoad(mDataset, strSrc))
    {
        LOG(ERROR)<<"Load File Error:"<<strSrc;
        return false;
    }

    Index<L2<float> > index(mDataset, flann::KDTreeIndexParams(4));
    index.buildIndex();                                                                               
    index.save(strIdxName.c_str());

    delete[] mDataset.ptr();

    return true;
}

bool bLoad(Matrix<float> & mDataset, const string& strFile)
{
    filebuf *fpBuf;
    long long lSize;
    char *pBuff;
    //要读入整个文件必须采用二进制打开
    ifstream fStr(strFile.c_str(), ios::binary);
    if(!fStr.is_open())
    {
        LOG(ERROR)<<"Open file error:"<<strFile;
        return false;
    }
    //获取fStr对应的buff对象的指针
    fpBuf=fStr.rdbuf();

    //调用buff对象方法获取文件大小
    lSize = fpBuf->pubseekoff(0, ios::end, ios::in);
    fpBuf->pubseekpos(0, ios::in);
    LOG(INFO)<<"lSize="<<lSize;

    if(lSize <= 0)
    {
        fStr.close();
        LOG(INFO)<<"lSize="<<lSize;
        return true;
    }
    //分配内存空间
    mDataset = flann::Matrix<float>(new float[lSize/sizeof(float)], lSize/(IMAGE_FEATURE_NUM*sizeof(float)), IMAGE_FEATURE_NUM);
    fpBuf->sgetn((char*)mDataset[0], lSize);
    //for test
    //for(int i=0; i< IMAGE_FEATURE_NUM; i++)
    //{
    //    LOG(INFO)<<mDataset[0][i];
    //}
    /////////////
    LOG(INFO)<<"IMAGE_FEATURE_NUM="<<IMAGE_FEATURE_NUM;
    LOG(INFO)<<"rows="<<lSize/(IMAGE_FEATURE_NUM*sizeof(float));

    fStr.close();
    return true;
}
/**
bool bLoad(Matrix<float> & mDataset, const string& strFile)
{
    ////////////////////////////
    size_t lSize;
    struct stat FileStat;
    SFeatureidx sFeatureidx;
    memset(&sFeatureidx, 0, sizeof(SFeatureidx));
    size_t nLen=0;

    FILE *fpSrcFileIdx = fopen((strFile+"idx").c_str(), "rb");
    if(NULL == fpSrcFileIdx)
    {
        LOG(ERROR)<<"open file error "<<(strFile+"idx").c_str();
        return false;
    }

    //读取索引文件, 结构为size_t类型的起始位置和长度
    if(-1==stat((strFile+"idx").c_str(), &FileStat))
    {
        LOG(ERROR)<<"filestat feature index error:"<<(strFile+"idx").c_str();
        fclose(fpSrcFileIdx);
        return false;
    }
    size_t nIdxSize=FileStat.st_size;
    //LOG(INFO)<<"nIdxSize=:"<<nIdxSize;
    int nIdxNum= nIdxSize/sizeof(SFeatureidx);
    //LOG(INFO)<<"nIdxNum=:"<<nIdxNum;
    //为索引分配内存空间
    char * pIdx = new char[nIdxSize];
    memset(pIdx, 0, nIdxSize);
    SFeatureidx *pFeatureidx = (SFeatureidx *)pIdx;

    if( fread(pIdx, nIdxSize, 1, fpSrcFileIdx) < 0 )
    {
        LOG(ERROR)<<"read index error:"<<(strFile+"idx").c_str();
        fclose(fpSrcFileIdx);
        return false;
    }
     
    ////////////////////////////

    //分配内存空间
    mDataset = flann::Matrix<float>((float *)(new char[sizeof(float)*nIdxNum*IMAGE_FEATURE_NUM]), nIdxNum, IMAGE_FEATURE_NUM);
    memset(mDataset[0], 0, sizeof(float)*nIdxNum*IMAGE_FEATURE_NUM);
    ///////////////
    //读取feat
    FILE *fpSrcFile= fopen(strFile.c_str(), "rb");
    if(NULL == fpSrcFile)
    {
        LOG(ERROR)<<"open file error "<<strFile.c_str();
        return false;
    }

    //申请内存
    size_t lRecLen = sizeof(float)*IMAGE_FEATURE_NUM;
    char * pFeature = new char[lRecLen];
    memset(pFeature, 0, lRecLen);
    //申请内存
    char * pUPFeature = new char[lRecLen];
    memset(pUPFeature, 0, lRecLen);

    size_t nOffset=0;
    size_t nRet=0;
    for(int i=0; i<nIdxNum; i++)
    {
        sFeatureidx = *(pFeatureidx + i);
        fseek(fpSrcFile, sFeatureidx.nPos, SEEK_SET);
//TODO confirm nLen vs lRecLen
        nRet = fread(pFeature, sFeatureidx.nLen, 1, fpSrcFile);
        if(nRet < 0)
        {
            LOG(ERROR)<<"read err";
        }
        //LOG(ERROR)<<"i="<<i;
        //LOG(INFO)<<"sFeatureidx.nPos=:"<<sFeatureidx.nPos;
        //LOG(INFO)<<"sFeatureidx.nLen=:"<<sFeatureidx.nLen;
        if( 0 != uncompressto((const char *)pFeature, pUPFeature, sFeatureidx.nLen, nLen) )
        {
            LOG(ERROR)<<"compress err";
            LOG(ERROR)<<"file:"<<strFile.c_str();
            return false;
        }
//check nLen vs IMAGE_FEATURE_NUM*sizeof(float)
        //LOG(ERROR)<<"nLen="<<nLen;
        memcpy((char*)mDataset[0]+nOffset, pUPFeature, nLen);
        //for test
        //LOG(INFO)<<mDataset[0][nOffset+100];
        /////////////
        nOffset += nLen;        
    }
    LOG(INFO)<<"rows="<<nIdxNum;

    delete []pIdx;
    delete []pFeature;
    delete []pUPFeature;
    if(fpSrcFile != NULL)
    {
        fclose(fpSrcFile);
        fpSrcFile = NULL;
    }
    if(fpSrcFileIdx != NULL)
    {
        fclose(fpSrcFileIdx);
        fpSrcFile = NULL;
    }

    return true;
}
**/

bool bZipData(const string &strSrc)
{
    string strZipFile=strSrc+".zp";
    string strIdxFile=strSrc+".zpidx";
    int err,fd=-1;
    size_t  num;

    //struct timeb t1;
    struct stat sb;
    size_t page_size= sysconf(_SC_PAGE_SIZE);

    fd= open(strSrc.c_str(), O_RDONLY) ;
    if(fd==-1)
    {
        LOG(ERROR)<<"open file Error"<<strSrc.c_str();
        return false;
    }

    if ((fstat(fd, &sb)) == -1)
    {
        LOG(ERROR)<<"fstat";
        return false;
    }
    num= sb.st_size/(sizeof(float)*IMAGE_FEATURE_NUM);

    fstream fdata(strZipFile.c_str(), ios_base::out|ios::binary);
    if (!fdata.is_open())
    {
        LOG(ERROR)<<"open zip file error"<<strZipFile.c_str();
        return false;
    }

    fstream findex(strIdxFile.c_str(), ios_base::out|ios::binary);
    if (!findex.is_open())
    {
        LOG(ERROR)<<"open idx file error"<<strIdxFile.c_str();
        return false;
    }

    //mmap
    char* pStart= NULL;
    off_t offTotal,offPart1,offPart2;
    size_t len= sizeof(float)*IMAGE_FEATURE_NUM;
    size_t comprLen;

    //zlib
    size_t lstart=0;
    char *compr= new char[len];
    for(int i=0; i<num; i++)
    {
        offTotal= i*len;
        offPart2= offTotal%page_size;
        offPart1= offTotal- offPart2;
        pStart= (char *)mmap(NULL, offPart2+len, PROT_READ, MAP_PRIVATE, fd, offPart1);

        if(pStart==MAP_FAILED)
        {
            LOG(ERROR)<<"mmap fail";
            return false;
        }

        //压缩
        comprLen= len;
        memset(compr, 0, comprLen);
        err = compress((Bytef*)compr, &comprLen, (const Bytef*)(pStart+offPart2), len);

        if (err != Z_OK) {
            LOG(ERROR)<< "compess error: " << err;
            return false;
        }

        //将压缩后的信息和索引写入文件
        fdata.seekp(0,ios_base::end);
        fdata.write(compr, comprLen);

        //记录压缩后每张图片特征起始位置
        findex.seekp(0,ios_base::end);
        findex.write((char*)&(lstart), sizeof(size_t));
        findex.seekp(0,ios_base::end);
        findex.write((char*)&(comprLen), sizeof(size_t));

        lstart+= comprLen;
        munmap(pStart, offPart2+len);

    }
    //写入最后一个图片的结束位置
    findex.close();
    fdata.close();
    close(fd);

    delete []compr;
    return true;
}

