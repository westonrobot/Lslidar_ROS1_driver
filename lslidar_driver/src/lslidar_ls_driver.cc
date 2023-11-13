/******************************************************************************
 * This file is part of lslidar_cx driver.
 *
 * Copyright 2022 LeiShen Intelligent Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

#include <lslidar_ls_driver/lslidar_ls_driver.h>
#include <algorithm>
#include <memory>

using namespace std::chrono;

namespace lslidar_ch_driver {
    LslidarChDriver::LslidarChDriver(
            ros::NodeHandle &n, ros::NodeHandle &pn) :
            nh(n),
            pnh(pn),
            socket_id(-1),
            min_range(0.15),
            max_range(200),
            publish_point_cloud(true),
            packet_end_time(0.0),
            current_packet_time(0.0),
            last_packet_time(0.0),
            use_time_service(false),
            return_mode(1),
            g_fAngleAcc_V(0.01),
            is_add_frame_(false),
            get_ms06_param(true),
            is_get_difop_(false), last_packet_number_(-1), current_packet_number_(0), total_packet_loss_(0),
            frame_count(0),
            threadPool_(std::make_unique<ThreadPool>(2)),
            point_cloud_xyzirt_(new pcl::PointCloud<VPoint>),
            point_cloud_xyzirt_bak_(new pcl::PointCloud<VPoint>),
            point_cloud_xyzirt_pub_(new pcl::PointCloud<VPoint>) {

        // create the sin and cos table for different azimuth and vertical values
        for (int j = 0; j < 36000; ++j) {
            double angle = static_cast<double>(j) * 0.01 * 0.017453293;
            sin_table[j] = sin(angle);
            cos_table[j] = cos(angle);
        }

        //double mirror_angle[4] = {0, -2, -1, -3};   //摆镜角度   //根据通道不同偏移角度不同
        double mirror_angle[4] = {1.5, -0.5, 0.5, -1.5};   //摆镜角度   //根据通道不同偏移角度不同
        for (int i = 0; i < 4; ++i) {
            cos_mirror_angle[i] = cos(DEG2RAD(mirror_angle[i]));
            sin_mirror_angle[i] = sin(DEG2RAD(mirror_angle[i]));
        }
    }

    LslidarChDriver::~LslidarChDriver() {
        if (nullptr == difop_thread_) {
            difop_thread_->join();
        }
        (void) close(socket_id);
    }

    bool LslidarChDriver::loadParameters() {
        pnh.param("frame_id", frame_id, std::string("lslidar"));
        pnh.param("lidar_ip", lidar_ip_string, std::string("192.168.1.222"));
        pnh.param<bool>("add_multicast", add_multicast, false);
        pnh.param("group_ip", group_ip_string, std::string("234.2.3.2"));
        pnh.param("msop_port", msop_udp_port, (int) MSOP_DATA_PORT_NUMBER);
        pnh.param("difop_port", difop_udp_port, (int) DIFOP_DATA_PORT_NUMBER);
        pnh.param("use_time_service", use_time_service, false);
        pnh.param("pcap", dump_file, std::string(""));

        inet_aton(lidar_ip_string.c_str(), &lidar_ip);
        // ROS_INFO_STREAM("Opening UDP socket: address " << lidar_ip_string);
        if (add_multicast) ROS_INFO_STREAM("Opening UDP socket: group_address " << group_ip_string);
        // ROS_INFO_STREAM("Opening UDP socket: port " << UDP_PORT_NUMBER);

        pnh.param<double>("min_range", min_range, 0.5);
        pnh.param<double>("max_range", max_range, 100.0);
        pnh.param<std::string>("pointcloud_topic", pointcloud_topic, "lslidar_point_cloud");
        pnh.param<bool>("publish_point_cloud", publish_point_cloud, true);
        pnh.param<std::string>("frame_id", frame_id, "lslidar");
        pnh.param<int>("scan_start_angle", scan_start_angle, -60);
        pnh.param<int>("scan_end_angle", scan_end_angle, 60);
        ROS_INFO("Using time service or not %d", use_time_service);

        return true;
    }

    bool LslidarChDriver::createRosIO() {
        point_cloud_pub = nh.advertise<sensor_msgs::PointCloud2>(pointcloud_topic, 1);
        packet_loss_pub = nh.advertise<std_msgs::Int64>("packet_loss", 1);

        double packet_rate = 11228;
        if (!dump_file.empty()) {
            msop_input_.reset(new lslidar_ch_driver::InputPCAP(pnh, msop_udp_port, packet_rate, dump_file));
            difop_input_.reset(new lslidar_ch_driver::InputPCAP(pnh, difop_udp_port, 1, dump_file));
        } else {
            msop_input_.reset(new lslidar_ch_driver::InputSocket(pnh, msop_udp_port));
            difop_input_.reset(new lslidar_ch_driver::InputSocket(pnh, difop_udp_port));
        }

        difop_thread_ = std::make_shared<std::thread>([this]() { difopPoll(); });
//        difop_thread_->detach();
        return true;
    }

    bool LslidarChDriver::initialize() {
        this->initTimeStamp();
        if (!loadParameters()) {
            ROS_ERROR("Cannot load all required ROS parameters...");
            return false;
        }

        if (!createRosIO()) {
            ROS_ERROR("Cannot create all ROS IO...");
            return false;
        }
        return true;
    }

    bool LslidarChDriver::polling() {
        // Allocate a new shared pointer for zero-copy sharing with other nodelets.
        lslidar_msgs::LslidarChPacketPtr packet(new lslidar_msgs::LslidarChPacket());

        // Since the lslidar delivers data at a very high rate, keep
        // reading and publishing scans as fast as possible.
        while (true) {
            // keep reading until full packet received
            int rc = msop_input_->getPacket(packet);
            if (rc == 0) {
                break;       // got a full packet?
            }
            if (rc < 0) return false; // end of file reached?
        }

        // publish message using time of last packet read
        if (use_time_service) {
            // it is already the msop msg
            // use the first packets
            lslidar_msgs::LslidarChPacket pkt = *packet;
            if (0xff == pkt.data[1194]) {    //ptp授时
                uint64_t timestamp_s = (pkt.data[1195] * 0 + (pkt.data[1196] << 24) +
                                        (pkt.data[1197] << 16) + (pkt.data[1198] << 8) + pkt.data[1199] * pow(2, 0));
                uint64_t timestamp_nsce = (pkt.data[1200] << 24) + (pkt.data[1201] << 16) +
                                          (pkt.data[1202] << 8) + (pkt.data[1203]);
                timeStamp = ros::Time(timestamp_s, timestamp_nsce);// s,ns
                packet->header.stamp = timeStamp;
                current_packet_time = packet->header.stamp.toSec();
            } else {          //gps授时
                this->packetTimeStamp[4] = pkt.data[1199];
                this->packetTimeStamp[5] = pkt.data[1198];
                this->packetTimeStamp[6] = pkt.data[1197];
                this->packetTimeStamp[7] = pkt.data[1196];
                this->packetTimeStamp[8] = pkt.data[1195];
                this->packetTimeStamp[9] = pkt.data[1194];
                struct tm cur_time{};
                memset(&cur_time, 0, sizeof(cur_time));
                cur_time.tm_sec = this->packetTimeStamp[4];
                cur_time.tm_min = this->packetTimeStamp[5];
                cur_time.tm_hour = this->packetTimeStamp[6];
                cur_time.tm_mday = this->packetTimeStamp[7];
                cur_time.tm_mon = this->packetTimeStamp[8] - 1;
                cur_time.tm_year = this->packetTimeStamp[9] + 2000 - 1900;
                this->pointcloudTimeStamp = static_cast<uint64_t>(timegm(&cur_time)); //s
                uint64_t packet_timestamp;
                packet_timestamp = pkt.data[1203] +
                                   (pkt.data[1202] << 8) +
                                   (pkt.data[1201] << 16) +
                                   (pkt.data[1200] << 24); //ns
                timeStamp = ros::Time(this->pointcloudTimeStamp, packet_timestamp);// s,ns
                packet->header.stamp = timeStamp;
                current_packet_time = packet->header.stamp.toSec();
            }
        } else {
            packet->header.stamp = ros::Time::now();
            current_packet_time = packet->header.stamp.toSec();
        }

        lslidarChPacketProcess(packet);
        return true;
    }

    void LslidarChDriver::initTimeStamp() {
        for (unsigned char &i : this->packetTimeStamp) {
            i = 0;
        }
        this->pointcloudTimeStamp = 0;
        this->timeStamp = ros::Time(0.0);
    }

    void LslidarChDriver::difopPoll() {
        lslidar_msgs::LslidarChPacketPtr difop_packet(new lslidar_msgs::LslidarChPacket());
        // reading and publishing scans as fast as possible.
        while (ros::ok()) {
            // keep reading
            int rc = difop_input_->getPacket(difop_packet);
            if (rc == 0) {
                if (difop_packet->data[0] == 0x00 || difop_packet->data[0] == 0xa5) {
                    if (difop_packet->data[1] == 0xff && difop_packet->data[2] == 0x00 &&
                        difop_packet->data[3] == 0x5a) {
                        if (difop_packet->data[231] == 64 || difop_packet->data[231] == 65) {
                            is_add_frame_ = true;
                        }
                        m_horizontal_point = difop_packet->data[184] * 256 + difop_packet->data[185];


                        int majorVersion = difop_packet->data[1202];
                        int minorVersion1 = difop_packet->data[1203] / 16;
                        int minorVersion2 = difop_packet->data[1203] % 16;

                        //v1.1 :0.01   //v1.2以后  ： 0.0025
                        if (1 > majorVersion || (1 == majorVersion && minorVersion1 > 1)) {
                            g_fAngleAcc_V = 0.0025;
                        } else {
                            g_fAngleAcc_V = 0.01;
                        }

                        float fInitAngle_V = difop_packet->data[188] * 256 + difop_packet->data[189];
                        if (fInitAngle_V > 32767) {
                            fInitAngle_V = fInitAngle_V - 65536;
                        }
                        this->prism_angle[0] = fInitAngle_V * g_fAngleAcc_V;

                        fInitAngle_V = difop_packet->data[190] * 256 + difop_packet->data[191];
                        if (fInitAngle_V > 32767) {
                            fInitAngle_V = fInitAngle_V - 65536;
                        }
                        this->prism_angle[1] = fInitAngle_V * g_fAngleAcc_V;

                        fInitAngle_V = difop_packet->data[192] * 256 + difop_packet->data[193];
                        if (fInitAngle_V > 32767) {
                            fInitAngle_V = fInitAngle_V - 65536;
                        }
                        this->prism_angle[2] = fInitAngle_V * g_fAngleAcc_V;

                        fInitAngle_V = difop_packet->data[194] * 256 + difop_packet->data[195];
                        if (fInitAngle_V > 32767) {
                            fInitAngle_V = fInitAngle_V - 65536;
                        }
                        this->prism_angle[3] = fInitAngle_V * g_fAngleAcc_V;
                        is_get_difop_ = true;
                    }
                }
            } else if (rc < 0) {
                return;
            }
            ros::spinOnce();
        }
    }

    void LslidarChDriver::publishPointCloudNew() {
        if (!is_get_difop_) return;
        std::unique_lock<std::mutex> lock(pc_mutex_);
        point_cloud_xyzirt_pub_->header.frame_id = frame_id;
        point_cloud_xyzirt_pub_->height = 1;
        sensor_msgs::PointCloud2 pc_msg;
        pcl::toROSMsg(*point_cloud_xyzirt_pub_, pc_msg);
        pc_msg.header.stamp = packet_timeStamp;
        point_cloud_pub.publish(pc_msg);
        ROS_DEBUG("pointcloud size: %u", pc_msg.width);
    }

    int LslidarChDriver::convertCoordinate(const struct Firing &lidardata) {
        if (!isPointInRange(lidardata.distance * g_fDistanceAcc)) {
            return -1;
        }
        if ((lidardata.azimuth < scan_start_angle) || (lidardata.azimuth > scan_end_angle)) {
            return -1;
        }
        double fAngle_H = 0.0;         //水平角度
        double fAngle_V = 0.0;         // 垂直角度
        fAngle_H = lidardata.azimuth;
        fAngle_V = lidardata.vertical_angle;

        //加畸变
        double fSinV_angle = 0;
        double fCosV_angle = 0;

        //振镜偏移角度 = 实际垂直角度 / 2  - 偏移值
        double fGalvanometrtAngle = 0;
        //fGalvanometrtAngle = (((fAngle_V + 0.05) / 0.8) + 1) * 0.46 + 6.72;
        //fGalvanometrtAngle = fAngle_V + 7.26;
        //fGalvanometrtAngle = fAngle_V + 6.37;
        fGalvanometrtAngle = fAngle_V + m_offset;

        while (fGalvanometrtAngle < 0.0) {
            fGalvanometrtAngle += 360.0;
        }
        while (fAngle_H < 0.0) {
            fAngle_H += 360.0;
        }

        int table_index_V = int(fGalvanometrtAngle * 100) % 36000;
        int table_index_H = int(fAngle_H * 100) % 36000;

        double fAngle_R0 = cos30 * cos_mirror_angle[lidardata.channel_number % 4] * cos_table[table_index_V] -
                           sin_table[table_index_V] * sin_mirror_angle[lidardata.channel_number % 4];

        fSinV_angle = 2 * fAngle_R0 * sin_table[table_index_V] + sin_mirror_angle[lidardata.channel_number % 4];
        fCosV_angle = sqrt(1 - pow(fSinV_angle, 2));

        double fSinCite = (2 * fAngle_R0 * cos_table[table_index_V] * sin30 -
                           cos_mirror_angle[lidardata.channel_number % 4] * sin60) / fCosV_angle;
        double fCosCite = sqrt(1 - pow(fSinCite, 2));

        double fSinCite_H = sin_table[table_index_H] * fCosCite + cos_table[table_index_H] * fSinCite;
        double fCosCite_H = cos_table[table_index_H] * fCosCite - sin_table[table_index_H] * fSinCite;

        double x_coord = 0.0, y_coord = 0.0, z_coord = 0.0;
        x_coord = (lidardata.distance * fCosV_angle * fSinCite_H) * g_fDistanceAcc;
        y_coord = (lidardata.distance * fCosV_angle * fCosCite_H) * g_fDistanceAcc;
        z_coord = (lidardata.distance * fSinV_angle) * g_fDistanceAcc;

        //pcl::PointXYZI point;
        VPoint point;
        point.x = x_coord;
        point.y = y_coord;
        point.z = z_coord;
        point.intensity = lidardata.intensity;
        point.ring = lidardata.channel_number;
        point.timestamp = lidardata.time;
        point_cloud_xyzirt_->points.push_back(point);
        ++point_cloud_xyzirt_->width;

        point_cloud_xyzirt_bak_->points.push_back(point);
        ++point_cloud_xyzirt_bak_->width;
        return 0;
    }

    void LslidarChDriver::lslidarChPacketProcess(const lslidar_msgs::LslidarChPacketPtr &msg) {
        struct Firing lidardata{};
        // Convert the msg to the raw packet type.
        packet_timeStamp = msg->header.stamp;
        packet_end_time = packet_timeStamp.toSec();
        bool packetType = false;
        if (msg->data[1205] == 0x02) {
            return_mode = 2;
        }
        if(get_ms06_param && m_horizontal_point != 0 && msg->data[1204] == 192){
          //ms06  param
            double mirror_angle[4] = {1.5, 0.5, -0.5, -1.5};   //摆镜角度   //根据通道不同偏移角度不同
            for (int i = 0; i < 4; ++i) {
                cos_mirror_angle[i] = cos(DEG2RAD(mirror_angle[i]));
                sin_mirror_angle[i] = sin(DEG2RAD(mirror_angle[i]));
            }
            m_offset = 10.82;
            g_fAngleAcc_V = 0.01;
            g_fDistanceAcc = 0.1 * 0.04;
            get_ms06_param = false;
        }

        if (return_mode == 1) {
            current_packet_number_ = (msg->data[1192] << 8) + msg->data[1193];
            tmp_packet_number_ = current_packet_number_;

            if(current_packet_number_ - last_packet_number_ < 0){current_packet_number_ += 65536;}

            if (current_packet_number_ - last_packet_number_ > 1  && last_packet_number_ != -1) {
                total_packet_loss_ += current_packet_number_ - last_packet_number_ - 1;
                ROS_DEBUG("packet loss = %lu", total_packet_loss_);
                std_msgs::Int64 loss_data;
                loss_data.data = total_packet_loss_;
                packet_loss_pub.publish(loss_data);
            }
            last_packet_number_ = tmp_packet_number_;

            double packet_interval_time =
                    (current_packet_time - last_packet_time) / (POINTS_PER_PACKET_SINGLE_ECHO / 8.0);
            for (size_t point_idx = 0; point_idx < POINTS_PER_PACKET_SINGLE_ECHO; point_idx += 8) {
                if ((msg->data[point_idx] == 0xff) && (msg->data[point_idx + 1] == 0xaa) &&
                    (msg->data[point_idx + 2] == 0xbb) && (msg->data[point_idx + 3] == 0xcc) &&
                    (msg->data[point_idx + 4] == 0xdd)) {
                    packetType = true;
                    frame_count++;
                } else {
                    // Compute the time of the point
                    double point_time;
                    if (last_packet_time > 1e-6) {
                        point_time = packet_end_time -
                                     packet_interval_time * ((POINTS_PER_PACKET_SINGLE_ECHO - point_idx) / 8 - 1);
                    } else {
                        point_time = current_packet_time;
                    }

                    memset(&lidardata, 0, sizeof(lidardata));
                    //水平角度
                    double fAngle_H = msg->data[point_idx + 1] + (msg->data[point_idx] << 8);
                    if (fAngle_H > 32767) {
                        fAngle_H = (fAngle_H - 65536);
                    }
                    lidardata.azimuth = fAngle_H * 0.01;
                    //垂直角度+通道号
                    int iTempAngle = msg->data[point_idx + 2];
                    int iChannelNumber = iTempAngle >> 6; //左移六位 通道号
                    int iSymmbol = (iTempAngle >> 5) & 0x01; //左移五位 符号位
                    double fAngle_V = 0.0;
                    if (1 == iSymmbol) // 符号位 0：正数 1：负数
                    {
                        int iAngle_V = msg->data[point_idx + 3] + (msg->data[point_idx + 2] << 8);

                        fAngle_V = iAngle_V | 0xc000;
                        if (fAngle_V > 32767) {
                            fAngle_V = (fAngle_V - 65536);
                        }
                    } else {
                        int iAngle_Hight = iTempAngle & 0x3f;
                        fAngle_V = msg->data[point_idx + 3] + (iAngle_Hight << 8);
                    }

                    lidardata.vertical_angle = fAngle_V * g_fAngleAcc_V;
                    lidardata.channel_number = iChannelNumber;
                    lidardata.distance = ((msg->data[point_idx + 4] << 16) + (msg->data[point_idx + 5] << 8) +
                                          msg->data[point_idx + 6]);
                    lidardata.intensity = msg->data[point_idx + 7];
                    lidardata.time = point_time;
                    lidardata.azimuth = fAngle_H * 0.01;
                    convertCoordinate(lidardata);
                }

                if (packetType) {
                    if (is_add_frame_) {
                        if (frame_count >= 2) {
                            {
                                std::unique_lock<std::mutex> lock(pc_mutex_);
                                point_cloud_xyzirt_pub_ = point_cloud_xyzirt_;
                            }
                            threadPool_->enqueue([&]() { publishPointCloudNew(); });
                        }
                        packetType = false;
                        point_cloud_xyzirt_ = point_cloud_xyzirt_bak_;
                        point_cloud_xyzirt_bak_.reset(new pcl::PointCloud<VPoint>);
                    } else {
                        {
                            std::unique_lock<std::mutex> lock(pc_mutex_);
                            point_cloud_xyzirt_pub_ = point_cloud_xyzirt_;
                        }
                        threadPool_->enqueue([&]() { publishPointCloudNew(); });
                        packetType = false;
                        point_cloud_xyzirt_.reset(new pcl::PointCloud<VPoint>);
                        point_cloud_xyzirt_bak_.reset(new pcl::PointCloud<VPoint>);
                    }
                }
            }
        } else {
            current_packet_number_ = 
                    msg->data[1188] * 0 + (msg->data[1189] << 32) + (msg->data[1190] << 24) + (msg->data[1191] << 16) +
                    (msg->data[1192] << 8) + msg->data[1193];
            if (current_packet_number_ - last_packet_number_ > 1 && last_packet_number_ != -1) {
                total_packet_loss_ += current_packet_number_ - last_packet_number_ - 1;
                ROS_DEBUG("packet loss = %lu", total_packet_loss_);
                std_msgs::Int64 loss_data;
                loss_data.data = total_packet_loss_;
                packet_loss_pub.publish(loss_data);
            }
            last_packet_number_ = current_packet_number_;

            double packet_interval_time =
                    (current_packet_time - last_packet_time) / (POINTS_PER_PACKET_DOUBLE_ECHO / 12.0);
            for (size_t point_idx = 0; point_idx < POINTS_PER_PACKET_DOUBLE_ECHO; point_idx += 12) {
                if ((msg->data[point_idx] == 0xff) && (msg->data[point_idx + 1] == 0xaa) &&
                    (msg->data[point_idx + 2] == 0xbb) && (msg->data[point_idx + 3] == 0xcc) &&
                    (msg->data[point_idx + 4] == 0xdd)) {
                    packetType = true;
                    frame_count++;
                } else {
                    // Compute the time of the point
                    double point_time;
                    if (last_packet_time > 1e-6) {
                        point_time = packet_end_time -
                                     packet_interval_time * ((POINTS_PER_PACKET_DOUBLE_ECHO - point_idx) / 12 - 1);
                    } else {
                        point_time = current_packet_time;
                    }
                    memset(&lidardata, 0, sizeof(lidardata));
                    //水平角度
                    double fAngle_H = msg->data[point_idx + 1] + (msg->data[point_idx] << 8);
                    if (fAngle_H > 32767) {
                        fAngle_H = (fAngle_H - 65536);
                    }
                    lidardata.azimuth = fAngle_H * 0.01;

                    //垂直角度+通道号
                    int iTempAngle = msg->data[point_idx + 2];
                    int iChannelNumber = iTempAngle >> 6; //左移六位 通道号
                    int iSymmbol = (iTempAngle >> 5) & 0x01; //左移五位 符号位
                    double fAngle_V = 0.0;
                    if (1 == iSymmbol) // 符号位 0：正数 1：负数
                    {
                        int iAngle_V = msg->data[point_idx + 3] + (msg->data[point_idx + 2] << 8);

                        fAngle_V = iAngle_V | 0xc000;
                        if (fAngle_V > 32767) {
                            fAngle_V = (fAngle_V - 65536);
                        }
                    } else {
                        int iAngle_Hight = iTempAngle & 0x3f;
                        fAngle_V = msg->data[point_idx + 3] + (iAngle_Hight << 8);
                    }

                    lidardata.vertical_angle = fAngle_V * g_fAngleAcc_V;
                    lidardata.channel_number = iChannelNumber;
                    lidardata.distance = ((msg->data[point_idx + 4] << 16) + (msg->data[point_idx + 5] << 8) +
                                          msg->data[point_idx + 6]);
                    lidardata.intensity = msg->data[point_idx + 7];
                    lidardata.time = point_time;
                    convertCoordinate(lidardata);  // 第一个点

                    lidardata.distance = ((msg->data[point_idx + 8] << 16) + (msg->data[point_idx + 9] << 8) +
                                          msg->data[point_idx + 10]);
                    lidardata.intensity = msg->data[point_idx + 11];
                    lidardata.time = point_time;
                    convertCoordinate(lidardata);  // 第二个点
                }

                if (packetType) {
                    if (is_add_frame_) {
                        if (frame_count >= 2) {
                            {
                                std::unique_lock<std::mutex> lock(pc_mutex_);
                                point_cloud_xyzirt_pub_ = point_cloud_xyzirt_;
                            }
                            threadPool_->enqueue([&]() { publishPointCloudNew(); });
                        }
                        packetType = false;
                        point_cloud_xyzirt_ = point_cloud_xyzirt_bak_;
                        point_cloud_xyzirt_bak_.reset(new pcl::PointCloud<VPoint>);
                    } else {
                        {
                            std::unique_lock<std::mutex> lock(pc_mutex_);
                            point_cloud_xyzirt_pub_ = point_cloud_xyzirt_;
                        }
                        threadPool_->enqueue([&]() { publishPointCloudNew(); });
                        packetType = false;
                        point_cloud_xyzirt_.reset(new pcl::PointCloud<VPoint>);
                        point_cloud_xyzirt_bak_.reset(new pcl::PointCloud<VPoint>);
                    }
                }
            }
        }
        last_packet_time = current_packet_time;
    }

} // namespace lslidar_driver
