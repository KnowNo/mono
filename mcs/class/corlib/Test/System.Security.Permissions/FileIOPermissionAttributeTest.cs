//
// FileIOPermissionAttributeTest.cs - NUnit Test Cases for FileIOPermissionAttribute
//
// Author:
//	Sebastien Pouliot (spouliot@motus.com)
//
// (C) 2003 Motus Technologies Inc. (http://www.motus.com)
//

using NUnit.Framework;
using System;
using System.IO;
using System.Reflection;
using System.Security;
using System.Security.Permissions;
using System.Text;

namespace MonoTests.System.Security.Permissions {

	[TestFixture]
	public class FileIOPermissionAttributeTest : Assertion {

		[Test]
		public void Default () 
		{
			FileIOPermissionAttribute a = new FileIOPermissionAttribute (SecurityAction.Assert);
			AssertNull ("Append", a.Append);
			AssertNull ("PathDiscovery", a.PathDiscovery);
			AssertNull ("Read", a.Read);
			AssertNull ("Write", a.Write);
			AssertEquals ("TypeId", a.ToString (), a.TypeId.ToString ());
			Assert ("Unrestricted", !a.Unrestricted);

			FileIOPermission perm = (FileIOPermission) a.CreatePermission ();
			AssertEquals ("CreatePermission-AllFiles", FileIOPermissionAccess.NoAccess, perm.AllFiles);
			AssertEquals ("CreatePermission-AllLocalFiles", FileIOPermissionAccess.NoAccess, perm.AllLocalFiles);
		}

		[Test]
		public void Action () 
		{
			FileIOPermissionAttribute a = new FileIOPermissionAttribute (SecurityAction.Assert);
			AssertEquals ("Action=Assert", SecurityAction.Assert, a.Action);
			a.Action = SecurityAction.Demand;
			AssertEquals ("Action=Demand", SecurityAction.Demand, a.Action);
			a.Action = SecurityAction.Deny;
			AssertEquals ("Action=Deny", SecurityAction.Deny, a.Action);
			a.Action = SecurityAction.InheritanceDemand;
			AssertEquals ("Action=InheritanceDemand", SecurityAction.InheritanceDemand, a.Action);
			a.Action = SecurityAction.LinkDemand;
			AssertEquals ("Action=LinkDemand", SecurityAction.LinkDemand, a.Action);
			a.Action = SecurityAction.PermitOnly;
			AssertEquals ("Action=PermitOnly", SecurityAction.PermitOnly, a.Action);
			a.Action = SecurityAction.RequestMinimum;
			AssertEquals ("Action=RequestMinimum", SecurityAction.RequestMinimum, a.Action);
			a.Action = SecurityAction.RequestOptional;
			AssertEquals ("Action=RequestOptional", SecurityAction.RequestOptional, a.Action);
			a.Action = SecurityAction.RequestRefuse;
			AssertEquals ("Action=RequestRefuse", SecurityAction.RequestRefuse, a.Action);
		}

		[Test]
		public void All () 
		{
			string filename = Assembly.GetCallingAssembly ().Location;
			FileIOPermissionAttribute attr = new FileIOPermissionAttribute (SecurityAction.Assert);
			attr.All = filename;
			AssertEquals ("All=Append", filename, attr.Append);
			AssertEquals ("All=PathDiscovery", filename, attr.PathDiscovery);
			AssertEquals ("All=Read", filename, attr.Read);
			AssertEquals ("All=Write", filename, attr.Write);
			FileIOPermission p = (FileIOPermission) attr.CreatePermission ();
			AssertEquals ("All=FileIOPermissionAttribute-Append", filename, p.GetPathList (FileIOPermissionAccess.Append)[0]);
			AssertEquals ("All=FileIOPermissionAttribute-PathDiscovery", filename, p.GetPathList (FileIOPermissionAccess.PathDiscovery)[0]);
			AssertEquals ("All=FileIOPermissionAttribute-Read", filename, p.GetPathList (FileIOPermissionAccess.Read)[0]);
			AssertEquals ("All=FileIOPermissionAttribute-Write", filename, p.GetPathList (FileIOPermissionAccess.Write)[0]);
		}
#if !NET_1_0
		[Test]
		[ExpectedException (typeof (NotSupportedException))]
		public void All_Get () 
		{
			FileIOPermissionAttribute attr = new FileIOPermissionAttribute (SecurityAction.Assert);
			string s = attr.All;
		}
#endif
		[Test]
		public void Append ()
		{
			string filename = Assembly.GetCallingAssembly ().Location;
			FileIOPermissionAttribute attr = new FileIOPermissionAttribute (SecurityAction.Assert);
			attr.Append = filename;
			AssertEquals ("Append=Append", filename, attr.Append);
			AssertNull ("PathDiscovery=null", attr.PathDiscovery);
			AssertNull ("Read=null", attr.Read);
			AssertNull ("Write=null", attr.Write);
			FileIOPermission p = (FileIOPermission) attr.CreatePermission ();
			AssertEquals ("Append=FileIOPermissionAttribute-Append", filename, p.GetPathList (FileIOPermissionAccess.Append)[0]);
			AssertNull ("Append=FileIOPermissionAttribute-PathDiscovery", p.GetPathList (FileIOPermissionAccess.PathDiscovery));
			AssertNull ("Append=FileIOPermissionAttribute-Read", p.GetPathList (FileIOPermissionAccess.Read));
			AssertNull ("Append=FileIOPermissionAttribute-Write", p.GetPathList (FileIOPermissionAccess.Write));
		}

		[Test]
		public void PathDiscovery () 
		{
			string filename = Assembly.GetCallingAssembly ().Location;
			FileIOPermissionAttribute attr = new FileIOPermissionAttribute (SecurityAction.Assert);
			attr.PathDiscovery = filename;
			AssertNull ("Append=null", attr.Append);
			AssertEquals ("PathDiscovery=PathDiscovery", filename, attr.PathDiscovery);
			AssertNull ("Read=null", attr.Read);
			AssertNull ("Write=null", attr.Write);
			FileIOPermission p = (FileIOPermission) attr.CreatePermission ();
			AssertNull ("PathDiscovery=FileIOPermissionAttribute-Append", p.GetPathList (FileIOPermissionAccess.Append));
			AssertEquals ("PathDiscovery=FileIOPermissionAttribute-PathDiscovery", filename, p.GetPathList (FileIOPermissionAccess.PathDiscovery)[0]);
			AssertNull ("PathDiscovery=FileIOPermissionAttribute-Read", p.GetPathList (FileIOPermissionAccess.Read));
			AssertNull ("PathDiscovery=FileIOPermissionAttribute-Write", p.GetPathList (FileIOPermissionAccess.Write));
		}

		[Test]
		public void Read () 
		{
			string filename = Assembly.GetCallingAssembly ().Location;
			FileIOPermissionAttribute attr = new FileIOPermissionAttribute (SecurityAction.Assert);
			attr.Read = filename;
			AssertNull ("Append=null", attr.Append);
			AssertNull ("PathDiscovery=null", attr.PathDiscovery);
			AssertEquals ("Read=Read", filename, attr.Read);
			AssertNull ("Write=null", attr.Write);
			FileIOPermission p = (FileIOPermission) attr.CreatePermission ();
			AssertNull ("PathDiscovery=FileIOPermissionAttribute-Append", p.GetPathList (FileIOPermissionAccess.Append));
			AssertNull ("PathDiscovery=FileIOPermissionAttribute-PathDiscovery", p.GetPathList (FileIOPermissionAccess.PathDiscovery));
			AssertEquals ("PathDiscovery=FileIOPermissionAttribute-Read", filename, p.GetPathList (FileIOPermissionAccess.Read)[0]);
			AssertNull ("PathDiscovery=FileIOPermissionAttribute-Write", p.GetPathList (FileIOPermissionAccess.Write));
		}

		[Test]
		public void Write () 
		{
			string filename = Assembly.GetCallingAssembly ().Location;
			FileIOPermissionAttribute attr = new FileIOPermissionAttribute (SecurityAction.Assert);
			attr.Write = filename;
			AssertNull ("Append=null", attr.Append);
			AssertNull ("PathDiscovery=null", attr.PathDiscovery);
			AssertNull ("Read=null", attr.Read);
			AssertEquals ("Write=Write", filename, attr.Write);
			FileIOPermission p = (FileIOPermission) attr.CreatePermission ();
			AssertNull ("PathDiscovery=FileIOPermissionAttribute-Append", p.GetPathList (FileIOPermissionAccess.Append));
			AssertNull ("PathDiscovery=FileIOPermissionAttribute-PathDiscovery", p.GetPathList (FileIOPermissionAccess.PathDiscovery));
			AssertNull ("PathDiscovery=FileIOPermissionAttribute-Read", p.GetPathList (FileIOPermissionAccess.Read));
			AssertEquals ("PathDiscovery=FileIOPermissionAttribute-Write", filename, p.GetPathList (FileIOPermissionAccess.Write)[0]);
		}

		[Test]
		public void Unrestricted () 
		{
			FileIOPermissionAttribute a = new FileIOPermissionAttribute (SecurityAction.Assert);
			a.Unrestricted = true;

			FileIOPermission perm = (FileIOPermission) a.CreatePermission ();
			Assert ("CreatePermission.IsUnrestricted", perm.IsUnrestricted ());
			AssertEquals ("CreatePermission.UsageAllowed", FileIOPermissionAccess.AllAccess, perm.AllFiles);
			AssertEquals ("CreatePermission.UserQuota", FileIOPermissionAccess.AllAccess, perm.AllLocalFiles);
		}
	}
}
