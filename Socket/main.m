//
//  main.m
//  Socket
//
//  Created by 共用 on 2014/04/25.
//  Copyright (c) 2014年 Panasonic System Networks Co., Ltd. 2013. All rights reserved.
//

#import <UIKit/UIKit.h>

#import "TestAppDelegate.h"


int get_ip_addr(char*);
void setVoip(int fd);

int main(int argc, char * argv[])
{
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([TestAppDelegate class]));
    }
}
